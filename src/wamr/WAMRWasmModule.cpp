#include <faabric/executor/Executor.h>
#include <faabric/scheduler/Scheduler.h>
#include <faabric/util/files.h>
#include <faabric/util/locks.h>
#include <faabric/util/logging.h>
#include <faabric/util/string_tools.h>
#include <storage/FileLoader.h>
#include <wamr/WAMRWasmModule.h>
#include <wamr/native.h>
#include <wasm/WasmExecutionContext.h>
#include <wasm/WasmModule.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <setjmp.h>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <aot_runtime.h>
#include <wasm_export.h>
#include <wasm_memory.h>
#include <wasm_runtime_common.h>

#define FAASM_WAMR_NUM_MAX_THREADS 200
#define NO_WASM_FUNC_PTR -1

namespace wasm {
// The high level API for WAMR can be found here:
// https://github.com/bytecodealliance/wasm-micro-runtime/blob/main/core/iwasm/include/wasm_export.h
static bool wamrInitialised = false;
const std::string WASI_PROC_EXIT = "Exception: wasi proc exit";

// WAMR maintains some global state, which we must be careful not to modify
// concurrently from our side. We are deliberately cautious with this locking,
// so it may cause performance issues under high churn of short-lived functions.
static std::mutex wamrGlobalsMutex;

/* RAII wrapper aroudn WAMR's thread enviornment (de)-initialisation.
 *
 * When using WAMR as a library from multiple threads, WAMR complains if
 * different steps of the runtime's lifecycle (e.g. initialisation, code gen,
 * module loading, memory instantiation, or function execution) are performed
 * by different threads. To fix this issue, we must make sure that each
 * thread's environment is properly initialized and (very importantly!)
 * destroyed after that. Failing to destroy it will create memory leaks in
 * TLS, leading to hard-to-debug segmentation faults.
 */
// Light reset: a thread's WAMR environment, once set up, is kept until the
// thread exits instead of being torn down after every call. Setting it up
// maps a signal stack, protects stack guard pages and installs signal
// handlers -- process-wide operations that queue on the memory-map lock and
// the signal lock with every other executor's.
struct WAMRThreadEnvKeeper
{
    bool kept = false;

    ~WAMRThreadEnvKeeper()
    {
        if (kept && wamrInitialised && wasm_runtime_thread_env_inited()) {
            wasm_runtime_destroy_thread_env();
        }
    }
};
static thread_local WAMRThreadEnvKeeper threadEnvKeeper;

class WAMRThreadEnv
{
  public:
    WAMRThreadEnv()
    {
        if (!wasm_runtime_thread_env_inited() &&
            !wasm_runtime_init_thread_env()) {
            SPDLOG_ERROR("Error intialisining thread env. for main thread");
            throw std::runtime_error("Error intiialising thread environment!");
        }
    }

    ~WAMRThreadEnv()
    {
        if (faabric::executor::lightResetEnabled()) {
            threadEnvKeeper.kept = true;
            return;
        }
        if (wasm_runtime_thread_env_inited()) {
            wasm_runtime_destroy_thread_env();
        }
        threadEnvKeeper.kept = false;
    }
};

void WAMRWasmModule::initialiseWAMRGlobally()
{
    faabric::util::UniqueLock lock(wamrGlobalsMutex);

    if (wamrInitialised) {
        return;
    }

    // Initialise WAMR runtime
    RuntimeInitArgs initArgs;
    memset(&initArgs, 0, sizeof(RuntimeInitArgs));

    // Memory configuration
    initArgs.mem_alloc_type = Alloc_With_Allocator;
    initArgs.mem_alloc_option.allocator.malloc_func = (void*)::malloc;
    initArgs.mem_alloc_option.allocator.realloc_func = (void*)::realloc;
    initArgs.mem_alloc_option.allocator.free_func = (void*)::free;
    initArgs.max_thread_num = FAASM_WAMR_NUM_MAX_THREADS;

    bool success = wasm_runtime_full_init(&initArgs);
    if (!success) {
        throw std::runtime_error("Failed to initialise WAMR");
    }

    SPDLOG_DEBUG("Successfully initialised WAMR");

    // Initialise Faasm's own native symbols
    initialiseWAMRNatives();

    // Set log level: BH_LOG_LEVEL_{FATAL,ERROR,WARNING,DEBUG,VERBOSE}
    bh_log_set_verbose_level(BH_LOG_LEVEL_WARNING);

    wamrInitialised = true;
}

void WAMRWasmModule::destroyWAMRGlobally()
{
    faabric::util::UniqueLock lock(wamrGlobalsMutex);

    if (!wamrInitialised) {
        return;
    }

    wasm_runtime_destroy();

    wamrInitialised = false;
}

WAMRWasmModule::WAMRWasmModule()
{
    initialiseWAMRGlobally();
}

WAMRWasmModule::WAMRWasmModule(int threadPoolSizeIn)
  : WasmModule(threadPoolSizeIn)
{
    initialiseWAMRGlobally();
}

WAMRWasmModule::~WAMRWasmModule()
{
    SPDLOG_TRACE(
      "Destructing WAMR wasm module {}/{}", boundUser, boundFunction);

    faabric::util::UniqueLock lock(wamrGlobalsMutex);

    // For WAMR's deinitialization procedure see here:
    // https://github.com/bytecodealliance/wasm-micro-runtime/tree/main/doc/embed_wamr.md#the-deinitialization-procedure
    destroyThreadsExecEnv(true);
    wasm_runtime_deinstantiate(moduleInstance);
    wasm_runtime_unload(wasmModule);
}

WAMRWasmModule* getExecutingWAMRModule()
{
    return reinterpret_cast<WAMRWasmModule*>(getExecutingModule());
}

// ----- Module lifecycle -----

// Reset is called once, from the main thread
void WAMRWasmModule::reset(faabric::Message& msg,
                           const std::string& snapshotKey)
{
    if (!_isBound) {
        return;
    }

    std::string funcStr = faabric::util::funcToString(msg, true);
    SPDLOG_DEBUG("WAMR resetting after {} (snap key {})", funcStr, snapshotKey);

    // Light reset: put the instance back in place. Re-instantiating maps a
    // fresh 8 GiB linear memory and unmaps the old one on every request,
    // and those mappings queue on the process-wide memory-map lock with
    // every other executor's -- and so do the page faults of the ones
    // running meanwhile. Restoring touches no mapping.
    if (faabric::executor::lightResetEnabled() && tryLightReset()) {
        faabric::scheduler::getScheduler().countLightReset();
        return;
    }

    destroyThreadsExecEnv(true);
    wasm_runtime_deinstantiate(moduleInstance);
    bindInternal(msg);
}

void WAMRWasmModule::captureCleanImage()
{
    cleanImage = CleanImage{};

    auto* inst = reinterpret_cast<AOTModuleInstance*>(moduleInstance);
    // One private memory, as AOT modules have; anything else re-instantiates.
    if (inst->memory_count != 1 || inst->memories == nullptr) {
        return;
    }
    AOTMemoryInstance* mem = inst->memories[0];
    if (mem == nullptr || mem->is_shared_memory ||
        mem->memory_data == nullptr) {
        return;
    }

    // Memory up to its last non-zero page: the rest of a fresh instance's
    // memory is zero, and a reset can zero it by dropping its pages.
    const uint64_t pageSize = static_cast<uint64_t>(getpagesize());
    const uint64_t memorySize = mem->memory_data_size;
    const uint8_t* base = mem->memory_data;
    uint64_t usedBytes = 0;
    for (uint64_t offset = 0; offset < memorySize; offset += pageSize) {
        uint64_t length = std::min(pageSize, memorySize - offset);
        const uint8_t* page = base + offset;
        bool zero =
          page[0] == 0 && std::memcmp(page, page + 1, length - 1) == 0;
        if (!zero) {
            usedBytes = offset + length;
        }
    }
    cleanImage.memory.assign(base, base + usedBytes);
    cleanImage.memorySize = memorySize;
    cleanImage.pageCount = mem->cur_page_count;
    cleanImage.bytesPerPage = mem->num_bytes_per_page;
    cleanImage.maxPageCount = mem->max_page_count;

    if (inst->global_data != nullptr && inst->global_data_size > 0) {
        cleanImage.globals.assign(inst->global_data,
                                  inst->global_data + inst->global_data_size);
    }
    for (uint32_t i = 0; i < inst->table_count; i++) {
        AOTTableInstance* table = inst->tables[i];
        if (table == nullptr) {
            return;
        }
        cleanImage.tables.emplace_back(table->elems,
                                       table->elems + table->cur_size);
    }

    cleanImage.valid = true;
    SPDLOG_DEBUG("WAMR clean image of {}/{}: {} of {} memory bytes, {} global "
                 "bytes, {} tables",
                 boundUser,
                 boundFunction,
                 usedBytes,
                 memorySize,
                 cleanImage.globals.size(),
                 cleanImage.tables.size());
}

bool WAMRWasmModule::tryLightReset()
{
    if (!cleanImage.valid || moduleInstance == nullptr) {
        return false;
    }
    auto* inst = reinterpret_cast<AOTModuleInstance*>(moduleInstance);
    AOTMemoryInstance* mem = inst->memories[0];

    // A memory that grew is shrunk back below (it keeps its pages mapped);
    // anything else of another shape re-instantiates.
    if (mem->cur_page_count < cleanImage.pageCount ||
        mem->memory_data_size < cleanImage.memorySize ||
        mem->num_bytes_per_page != cleanImage.bytesPerPage ||
        mem->max_page_count != cleanImage.maxPageCount ||
        inst->global_data_size != cleanImage.globals.size() ||
        inst->table_count != cleanImage.tables.size()) {
        return false;
    }
    for (uint32_t i = 0; i < inst->table_count; i++) {
        if (inst->tables[i]->cur_size != cleanImage.tables[i].size()) {
            return false;
        }
    }
    // A run that trapped may have left the instance anywhere; only a normal
    // return or exit is put back in place.
    const char* exception = wasm_runtime_get_exception(moduleInstance);
    if (exception != nullptr && std::string(exception) != WASI_PROC_EXIT) {
        return false;
    }

    // Memory: what was non-zero copied back, the rest -- grown pages too --
    // zeroed by dropping its pages (they fault back in as zero pages).
    // Neither changes a mapping.
    uint8_t* base = mem->memory_data;
    uint64_t copied = cleanImage.memory.size();
    uint64_t usedSize = mem->memory_data_size;
    if (copied > 0) {
        std::memcpy(base, cleanImage.memory.data(), copied);
    }
    if (copied < usedSize &&
        madvise(base + copied, usedSize - copied, MADV_DONTNEED) != 0) {
        SPDLOG_WARN("WAMR light reset of {}/{} could not drop pages ({}), "
                    "re-instantiating",
                    boundUser,
                    boundFunction,
                    errno);
        return false;
    }

    // A memory that grew goes back to its first size. Its grown pages stay
    // readable and writable, so that growing again only re-protects them as
    // they are -- unmapping them would take the memory-map lock for longer
    // -- and access beyond the size does not trap there until the instance
    // is re-instantiated: fine for trusted functions, not as isolation.
    if (usedSize > cleanImage.memorySize) {
        mem->cur_page_count = cleanImage.pageCount;
        SET_LINEAR_MEMORY_SIZE(mem,
                               static_cast<uint32_t>(cleanImage.memorySize));
        mem->memory_data_end = base + cleanImage.memorySize;
        wasm_runtime_set_mem_bound_check_bytes(mem, cleanImage.memorySize);
    }

    // Globals (the stack pointer among them) and tables.
    if (!cleanImage.globals.empty()) {
        std::memcpy(inst->global_data,
                    cleanImage.globals.data(),
                    cleanImage.globals.size());
    }
    for (uint32_t i = 0; i < inst->table_count; i++) {
        const auto& elems = cleanImage.tables[i];
        if (!elems.empty()) {
            std::memcpy(inst->tables[i]->elems,
                        elems.data(),
                        elems.size() * sizeof(uint32_t));
        }
    }

    // The host side of the run: any threads' environments, the exit and the
    // exit code. The main thread's environment is kept, as WAMR allows.
    destroyThreadsExecEnv(false);
    wasm_runtime_clear_exception(moduleInstance);
    wasm_runtime_get_wasi_ctx(moduleInstance)->exit_code = 0;
    currentBrk.store(getMemorySizeBytes(), std::memory_order_release);
    return true;
}

void WAMRWasmModule::doBindToFunction(faabric::Message& msg, bool cache)
{
    SPDLOG_TRACE("WAMR binding to {}/{} via message {}",
                 msg.user(),
                 msg.function(),
                 msg.id());

    // Load the wasm file
    storage::FileLoader& functionLoader = storage::getFileLoader();
    // wasmBytes = functionLoader.loadFunctionWamrAotFile(msg);

    // {
    //     faabric::util::UniqueLock lock(wamrGlobalsMutex);
    //     SPDLOG_TRACE("WAMR loading {} wasm bytes\n", wasmBytes.size());
    //     wasmModule = wasm_runtime_load(
    //       wasmBytes.data(), wasmBytes.size(), errorBuffer,
    //       ERROR_BUFFER_SIZE);

    //     if (wasmModule == nullptr) {
    //         std::string errorMsg = std::string(errorBuffer);
    //         SPDLOG_ERROR("Failed to load WAMR module: \n{}", errorMsg);
    //         throw std::runtime_error("Failed to load WAMR module");
    //     }
    // }

    for (int i = 0; i < 2; ++i) {
        // On the second attempt (i.e., i > 0), we first delete the local
        // file to force a re-download from S3.
        if (i > 0) {
            functionLoader.invalidateFunctionAotFile(msg);
        }

        // Load the wasm file bytes (from cache or S3)
        wasmBytes = functionLoader.loadFunctionWamrAotFile(msg);

        {
            faabric::util::UniqueLock lock(wamrGlobalsMutex);
            SPDLOG_TRACE("WAMR loading {} wasm bytes (attempt {}/2)",
                         wasmBytes.size(),
                         i + 1);
            wasmModule = wasm_runtime_load(wasmBytes.data(),
                                           wasmBytes.size(),
                                           errorBuffer,
                                           ERROR_BUFFER_SIZE);

            // If the module loaded successfully, break the retry loop.
            if (wasmModule != nullptr) {
                break;
            }

            // --- Loading failed, check the error ---
            std::string errorMsg = std::string(errorBuffer);

            // Check if it's the specific "unexpect end" error and if this is
            // the first attempt.
            if (i == 0 && errorMsg.find("unexpect") != std::string::npos) {
                SPDLOG_WARN(
                  "Module failed with 'unexpect end'. Retrying from S3.");
                // Continue to the next iteration to retry
                continue;
            } else {
                // On any other error, or if the retry fails, throw.
                SPDLOG_ERROR("Failed to load WAMR module (attempt {}/2): \n{}",
                             i + 1,
                             errorMsg);
                throw std::runtime_error("Failed to load WAMR module");
            }
        }
    }

    bindInternal(msg);
}

void WAMRWasmModule::bindInternal(faabric::Message& msg)
{
    // Prepare the filesystem. TODO - The opened files are never closed, which
    // can cause leaks.
    if (!filesystemPrepared) {
        filesystem.prepareFilesystem();
        filesystemPrepared = true;
    }

    // RAII-handle around WAMR's thread environment
    WAMRThreadEnv threadEnv;

    // Instantiate module. Set the app-managed heap size to 0 to use
    // wasi-libc's managed heap. See:
    // https://bytecodealliance.github.io/wamr.dev/blog/understand-the-wamr-heap/
    moduleInstance = wasm_runtime_instantiate(
      wasmModule, STACK_SIZE_KB, 0, errorBuffer, ERROR_BUFFER_SIZE);

    if (moduleInstance == nullptr) {
        SPDLOG_ERROR("WAMR module instantiation failed: {}", errorBuffer);
        throw std::runtime_error("WAMR module instantiation failed!");
    }

    // Sense-check the module
    auto* aotModule = reinterpret_cast<AOTModuleInstance*>(moduleInstance);
    AOTMemoryInstance* aotMem = ((AOTMemoryInstance**)aotModule->memories)[0];

    if (aotMem->num_bytes_per_page != WASM_BYTES_PER_PAGE) {
        SPDLOG_ERROR("WAMR module bytes per page wrong, {} != {}, overriding",
                     aotMem->num_bytes_per_page,
                     WASM_BYTES_PER_PAGE);
        throw std::runtime_error("WAMR module bytes per page wrong");
    }

    if (moduleInstance == nullptr) {
        std::string errorMsg = std::string(errorBuffer);
        SPDLOG_ERROR("Failed to instantiate WAMR module: \n{}", errorMsg);
        throw std::runtime_error("Failed to instantiate WAMR module");
    }
    currentBrk.store(getMemorySizeBytes(), std::memory_order_release);

    // In WAMR the thread stacks are managed by the runtime, not by us, in
    // a dynamic fashion that also guarantees no overflows. As a consequence,
    // we do not need to use Faasm's home-built thread stack management
    threadStacks = std::vector<uint32_t>(threadPoolSize, 0);

    // Create the execution environemnt for all the potential threads
    execEnvs = std::vector<WASMExecEnv*>(threadPoolSize, nullptr);

    // Execution environment for the main thread
    execEnvs.at(0) =
      wasm_runtime_create_exec_env(moduleInstance, STACK_SIZE_KB);
    if (execEnvs.at(0) == nullptr) {
        SPDLOG_ERROR("Error allocating execution environment for main thread!");
    }

    // Fresh, and nothing has run yet: what a light reset puts back.
    if (faabric::executor::lightResetEnabled() && !cleanImage.valid) {
        captureCleanImage();
    }
}

void WAMRWasmModule::createThreadsExecEnv(WASMExecEnv* parentExecEnv)
{
    if (parentExecEnv != execEnvs.at(0)) {
        SPDLOG_ERROR(
          "Creating thread's execution environment from non-main thread!");
        throw std::runtime_error("Creating threads from non-main thread!");
    }

    for (int i = 1; i < threadPoolSize; i++) {
        if (execEnvs.at(i) == nullptr) {
            wasm_runtime_set_exec_env_tls(parentExecEnv);
            execEnvs.at(i) = wasm_runtime_spawn_exec_env(execEnvs.at(0));
        }
        if (execEnvs.at(i) == nullptr) {
            SPDLOG_ERROR(
              "Error allocating execution environment for thread {}!", i);
            throw std::runtime_error("Error allocating exec. env!");
        }
    }
}

// Note that exec envs [1, threadPoolSize) are created using the spawn API,
// whereas exec env 0 is created using the normal create/destroy API. As
// a consequence, each must be destroyed with the corresponding API
void WAMRWasmModule::destroyThreadsExecEnv(bool destroyMainExecEnv)
{
    for (int i = 1; i < execEnvs.size(); i++) {
        if (execEnvs.at(i) != nullptr) {
            wasm_runtime_destroy_spawned_exec_env(execEnvs.at(i));
            execEnvs.at(i) = nullptr;
        }
    }

    if (destroyMainExecEnv && execEnvs.at(0) != nullptr) {
        wasm_runtime_destroy_exec_env(execEnvs.at(0));
        execEnvs.at(0) = nullptr;
        wasm_runtime_set_exec_env_tls(nullptr);
    }
}

int32_t WAMRWasmModule::executeFunction(faabric::Message& msg)
{
    SPDLOG_DEBUG("WAMR executing message {}", msg.id());

    // If we are calling this function, we know we are thread pool 0
    int thisThreadPoolIdx = 0;

    // Make sure context is set
    WasmExecutionContext ctx(this);
    int returnValue = 0;

    if (msg.funcptr() > 0) {
        // Run the function from the pointer
        returnValue = executeWasmFunctionFromPointer(thisThreadPoolIdx, msg);
    } else {
        prepareArgcArgv(msg);

        // Run the main function
        returnValue = executeWasmFunction(thisThreadPoolIdx, ENTRY_FUNC_NAME);

        // When running the main function (_start in WASI) we want to overwrite
        // the function's return value for the one in WAMR's WASI context.
        // The former is just the return value of _start, whereas the latter
        // is the actual return value of the entrypoint (e.g. main)
        returnValue = wasm_runtime_get_wasi_ctx(moduleInstance)->exit_code;
    }

    // Record the return value
    msg.set_returnvalue(returnValue);

    return returnValue;
}

int32_t WAMRWasmModule::executeBatchFunction(faabric::BatchExecuteRequest& req)
{
    SPDLOG_DEBUG("WAMR executing batch message with {}", req.appid());

    // If we are calling this function, we know we are thread pool 0
    int thisThreadPoolIdx = 0;

    // Make sure context is set
    WasmExecutionContext ctx(this);
    int returnValue = 0;

    if (req.messages(0).funcptr() > 0) {
        SPDLOG_ERROR("Function pointers not supported in batch mode");
        throw std::runtime_error(
          "Function pointers not supported in batch mode");
    } else {
        // We use the first message to gain argc and argv. But it shouldn't be
        // right. We should limit the cmdline be empty and the Argc and Argv are
        // empty.
        prepareArgcArgv(req.messages(0));

        // Run the main function
        returnValue = executeWasmFunction(thisThreadPoolIdx, ENTRY_FUNC_NAME);

        // When running the main function (_start in WASI) we want to overwrite
        // the function's return value for the one in WAMR's WASI context.
        // The former is just the return value of _start, whereas the latter
        // is the actual return value of the entrypoint (e.g. main)
        returnValue = wasm_runtime_get_wasi_ctx(moduleInstance)->exit_code;
    }

    // Record the return value. It is also recorded by the
    // WasmModule::executeBatchTask.
    for (int i = 0; i < req.messages_size(); i++) {
        req.mutable_messages(i)->set_returnvalue(returnValue);
    }

    return returnValue;
}

AOTFuncType* getFuncTypeFromFuncPtr(WASMModuleCommon* wasmModule,
                                    WASMModuleInstanceCommon* moduleInstance,
                                    int32_t wasmFuncPtr)
{
    assert(wasmModule != nullptr);
    assert(moduleInstance != nullptr);

    AOTModuleInstance* aotModuleInstance =
      reinterpret_cast<AOTModuleInstance*>(moduleInstance);
    AOTTableInstance* tableInstance = aotModuleInstance->tables[0];
    if (tableInstance == nullptr || wasmFuncPtr >= tableInstance->cur_size) {
        SPDLOG_ERROR("Error getting WAMR function signature from ptr: {}",
                     wasmFuncPtr);
        throw std::runtime_error("Error getting WAMR function signature");
    }
    uint32_t funcIdx = tableInstance->elems[wasmFuncPtr];
    uint32_t funcTypeIdx = aotModuleInstance->func_type_indexes[funcIdx];

    AOTModule* aotModule = reinterpret_cast<AOTModule*>(wasmModule);

    return aotModule->func_types[funcTypeIdx];
}

int32_t WAMRWasmModule::executeOMPThread(int threadPoolIdx,
                                         uint32_t stackTop,
                                         faabric::Message& msg)
{
    auto funcStr = faabric::util::funcToString(msg, false);
    int wasmFuncPtr = msg.funcptr();
    SPDLOG_DEBUG("Executing OpenMP thread {} for {}", threadPoolIdx, funcStr);

    auto* execEnv = execEnvs.at(threadPoolIdx);
    if (execEnvs.at(threadPoolIdx) == nullptr) {
        SPDLOG_ERROR("Exec. env not set for thread: {}!", threadPoolIdx);
        throw std::runtime_error("Thread execution environment not set!");
    }

    auto* moduleInstance = wasm_runtime_get_module_inst(execEnv);
    auto* wasmModule = wasm_runtime_get_module(moduleInstance);

    auto ompLevel = threads::getCurrentOpenMPLevel();
    if (ompLevel == nullptr) {
        SPDLOG_ERROR("null OMP level!");
    }

    // Set up function args
    // NOTE: an OpenMP microtask takes the following arguments:
    // - The thread ID within its current team
    // - The number of non-global shared variables it has access to
    // - A pointer to each of the non-global shared variables
    int argc = 2 + ompLevel->nSharedVarOffsets;
    std::vector<uint32_t> argv(argc);
    argv[0] = { (uint32_t)msg.appidx() };
    argv[1] = { (uint32_t)ompLevel->nSharedVarOffsets };

    // The rest of the arguments are the ones corresponding to OpenMP
    for (int i = 0; i < ompLevel->nSharedVarOffsets; i++) {
        argv.at(i + 2) = ompLevel->sharedVarOffsets[i];
    }

    // Work-out if the function reutrns a value or returns void
    AOTFuncType* funcType =
      getFuncTypeFromFuncPtr(wasmModule, moduleInstance, wasmFuncPtr);
    bool returnsVoid = funcType->result_count == 0;

    auto originalArgv = argv;
    bool success =
      executeCatchException(threadPoolIdx, nullptr, wasmFuncPtr, argc, argv);

    if (!success) {
        SPDLOG_ERROR("Error executing OpenMP thread {} func {}: {}",
                     threadPoolIdx,
                     wasmFuncPtr,
                     wasm_runtime_get_exception(moduleInstance));
        throw std::runtime_error(
          "Error executing WASM OpenMP func ptr with WAMR");
    }

    // If we are calling a void function by pointer with some arguments, the
    // return value will be, precisely, the input arguments (and not argv[0])
    uint32_t returnValue = argv[0];
    if (returnsVoid) {
        returnValue = !(argv[0] == originalArgv[0]);
    }

    SPDLOG_DEBUG(
      "WAMR finished executing OMP thread {} for {}", threadPoolIdx, funcStr);
    return returnValue;
}

int32_t WAMRWasmModule::executePthread(int threadPoolIdx,
                                       uint32_t stackTop,
                                       faabric::Message& msg)
{
    auto funcStr = faabric::util::funcToString(msg, false);
    int wasmFuncPtr = msg.funcptr();
    SPDLOG_DEBUG("WAMR executing Pthread {} for {}", threadPoolIdx, funcStr);

    auto* execEnv = execEnvs.at(threadPoolIdx);
    if (execEnvs.at(threadPoolIdx) == nullptr) {
        SPDLOG_ERROR("Exec. env not set for thread: {}!", threadPoolIdx);
        throw std::runtime_error("Thread execution environment not set!");
    }

    auto* moduleInstance = wasm_runtime_get_module_inst(execEnv);
    auto* wasmModule = wasm_runtime_get_module(moduleInstance);

    int argsPtr = std::stoi(msg.inputdata());
    std::vector<uint32_t> argv = { (uint32_t)argsPtr };

    // Work-out if the function reutrns a value or returns void
    AOTFuncType* funcType =
      getFuncTypeFromFuncPtr(wasmModule, moduleInstance, wasmFuncPtr);
    bool returnsVoid = funcType->result_count == 0;

    auto originalArgv = argv;
    bool success = executeCatchException(
      threadPoolIdx, nullptr, wasmFuncPtr, argv.size(), argv);

    if (!success) {
        SPDLOG_ERROR("Error executing Pthread func {}: {}",
                     wasmFuncPtr,
                     wasm_runtime_get_exception(moduleInstance));
        throw std::runtime_error(
          "Error executing WASM Pthread func ptr with WAMR");
    }

    // If we are calling a void function by pointer with some arguments, the
    // return value will be, precisely, the input arguments (and not argv[0])
    uint32_t returnValue = argv[0];
    if (returnsVoid) {
        returnValue = !(argv[0] == originalArgv[0]);
    }

    SPDLOG_DEBUG("WAMR finished executing Pthread thread {} for {} (ret: {})",
                 threadPoolIdx,
                 funcStr,
                 returnValue);
    return returnValue;
}

int WAMRWasmModule::executeWasmFunctionFromPointer(int threadPoolIdx,
                                                   faabric::Message& msg)
{
    // WASM function pointers are indices into the module's function table
    int wasmFuncPtr = msg.funcptr();
    std::string inputData = msg.inputdata();

    SPDLOG_DEBUG("WAMR executing function from pointer {} (args: {})",
                 wasmFuncPtr,
                 inputData);

    AOTFuncType* funcType =
      getFuncTypeFromFuncPtr(wasmModule, moduleInstance, wasmFuncPtr);
    int argCount = funcType->param_count;
    int resultCount = funcType->result_count;
    SPDLOG_DEBUG("WAMR Function pointer has {} arguments and returns {} value",
                 argCount,
                 resultCount);
    bool returnsVoid = resultCount == 0;

    // NOTE: WAMR doesn't provide a nice interface for calling functions using
    // function pointers, so we have to call a few more low-level functions to
    // get it to work.

    std::vector<uint32_t> argv;
    switch (argCount) {
        // Even if the function takes no arguments, we need to pass an argv
        // with at least one element, as WAMR will set the return value in
        // argv[0]
        case 0:
            argv = { 0 };
            break;
        // If we are calling with just one argument, we assume its an integer
        // value. We could switch on the data type of the AOTType*, but we
        // don't do it just yet
        case 1:
            argv = { (uint32_t)std::stoi(inputData) };
            break;
        default: {
            SPDLOG_ERROR("Unrecognised WAMR function pointer signature (args: "
                         "{}, return: {})",
                         argCount,
                         resultCount);
            throw std::runtime_error(
              "Unrecognised WAMR function pointer signature");
        }
    }
    std::vector<uint32_t> originalArgv = argv;
    bool success = executeCatchException(
      threadPoolIdx, nullptr, wasmFuncPtr, argCount, argv);

    if (!success) {
        SPDLOG_ERROR("Error executing {}: {}",
                     wasmFuncPtr,
                     wasm_runtime_get_exception(moduleInstance));
        throw std::runtime_error("Error executing WASM func ptr with WAMR");
    }

    // If we are calling a void function by pointer with some arguments, the
    // return value will be, precisely, the input arguments (and not 0)
    uint32_t returnValue;
    if (returnsVoid) {
        returnValue = !(argv[0] == originalArgv[0]);
    } else {
        returnValue = 0;
    }

    SPDLOG_DEBUG("WAMR finished executing func ptr {}", wasmFuncPtr);
    return returnValue;
}

int WAMRWasmModule::executeWasmFunction(int threadPoolIdx,
                                        const std::string& funcName)
{
    SPDLOG_DEBUG("WAMR executing function from string {}", funcName);

    WASMFunctionInstanceCommon* func =
      wasm_runtime_lookup_function(moduleInstance, funcName.c_str(), nullptr);
    if (func == nullptr) {
        SPDLOG_ERROR("Did not find function {} for module {}/{}",
                     funcName,
                     boundUser,
                     boundFunction);
        throw std::runtime_error("Did not find named wasm function");
    }

    // Note, for some reason WAMR sets the return value in the argv array you
    // pass it, therefore we should provide a single integer argv even though
    // it's not actually used
    std::vector<uint32_t> argv = { 0 };
    bool success =
      executeCatchException(threadPoolIdx, func, NO_WASM_FUNC_PTR, 0, argv);
    uint32_t returnValue = argv[0];

    if (!success) {
        SPDLOG_ERROR("Error executing {}: {}",
                     funcName,
                     wasm_runtime_get_exception(moduleInstance));
        throw std::runtime_error("Error executing WASM function with WAMR");
    }

    SPDLOG_DEBUG("WAMR finished executing {}", funcName);
    return returnValue;
}

// Low-level method to call a WASM function in WAMR and catch any thrown
// exceptions. This method is shared both if we call a function by pointer or
// by name
bool WAMRWasmModule::executeCatchException(int threadPoolIdx,
                                           WASMFunctionInstanceCommon* func,
                                           int wasmFuncPtr,
                                           int argc,
                                           std::vector<uint32_t>& argv)
{
    bool isIndirect;
    if (wasmFuncPtr == NO_WASM_FUNC_PTR && func != nullptr) {
        isIndirect = false;
    } else if (wasmFuncPtr != NO_WASM_FUNC_PTR && func == nullptr) {
        isIndirect = true;
    } else {
        throw std::runtime_error(
          "Incorrect combination of arguments to execute WAMR function");
    }

    // Prepare thread execution environment
    WASMExecEnv* thisThreadExecEnv = execEnvs.at(threadPoolIdx);
    if (thisThreadExecEnv == nullptr) {
        SPDLOG_ERROR("Null execution environment for thread: {}!",
                     threadPoolIdx);
        throw std::runtime_error("Null execution environment!");
    }

    // RAII handle around WAMR's thread env.
    WAMRThreadEnv threadEnv;

    // Initialise thread environment
    bool success;
    {
        // This switch statement is used to catch exceptions thrown by native
        // functions (written in C++) called from WASM code executed in WAMR.
        // Given that WAMR is written in C, exceptions are not propagated, and
        // thus we implement our custom handler
        switch (setjmp(wamrExceptionJmpBuf)) {
            case 0: {
                if (isIndirect) {
                    success = wasm_runtime_call_indirect(
                      thisThreadExecEnv, wasmFuncPtr, argc, argv.data());
                } else {
                    success = wasm_runtime_call_wasm(
                      thisThreadExecEnv, func, argc, argv.data());
                }
                break;
            }
            // Make sure that we throw an exception if setjmp is called from
            // a longjmp (and returns a value different than 0) as local
            // variables in the stack could be corrupted
            case WAMRExceptionTypes::FunctionMigratedException: {
                throw faabric::util::FunctionMigratedException(
                  "Migrating MPI rank");
            }
            case WAMRExceptionTypes::QueueTimeoutException: {
                throw std::runtime_error("Timed-out dequeueing!");
            }
            case WAMRExceptionTypes::DefaultException: {
                throw std::runtime_error("Default WAMR exception");
            }
            default: {
                SPDLOG_ERROR("WAMR exception handler reached unreachable case");
                throw std::runtime_error("Unreachable WAMR exception handler");
            }
        }
    }

    // Report "wasi proc exit" as success
    if (!success) {
        const char* exceptionPtr = wasm_runtime_get_exception(moduleInstance);
        if (exceptionPtr != nullptr &&
            (std::string(exceptionPtr) == WASI_PROC_EXIT)) {
            success = true;
        }
    }

    return success;
}

// -----
// Exception handling
// -----

void WAMRWasmModule::doThrowException(std::exception& e)
{
    // Switch over the different exception types we support. Unfortunately,
    // the setjmp/longjmp mechanism to catch C++ exceptions only lets us
    // change the return value of setjmp, but we can't propagate the string
    // associated to the exception
    if (dynamic_cast<faabric::util::FunctionMigratedException*>(&e) !=
        nullptr) {
        // Make sure to explicitly call the exceptions destructor explicitly
        // to avoid memory leaks when longjmp-ing
        e.~exception();
        longjmp(wamrExceptionJmpBuf,
                WAMRExceptionTypes::FunctionMigratedException);
    } else if (dynamic_cast<faabric::util::QueueTimeoutException*>(&e) !=
               nullptr) {
        e.~exception();
        longjmp(wamrExceptionJmpBuf, WAMRExceptionTypes::QueueTimeoutException);
    } else {
        e.~exception();
        longjmp(wamrExceptionJmpBuf, WAMRExceptionTypes::DefaultException);
    }
}

// -----
// Helper functions
// -----

// Validate that a memory range defined by a pointer and a size is a valid
// offset in the module's WASM linear memory.
void WAMRWasmModule::validateNativePointer(void* nativePtr, int size)
{
    bool success =
      wasm_runtime_validate_native_addr(moduleInstance, nativePtr, size);

    if (!success) {
        auto ex = std::runtime_error("Failed validating native pointer!");
        getExecutingModule()->doThrowException(ex);
    }
}

void WAMRWasmModule::writeStringToWasmMemory(const std::string& strHost,
                                             char* strWasm)
{
    validateNativePointer(strWasm, strHost.size());
    std::copy(strHost.begin(), strHost.end(), strWasm);
}

void WAMRWasmModule::writeWasmEnvToWamrMemory(uint32_t* envOffsetsWasm,
                                              char* envBuffWasm)
{
    writeStringArrayToMemory(
      wasmEnvironment.getVars(), envOffsetsWasm, envBuffWasm);
}

void WAMRWasmModule::validateWasmOffset(uint32_t wasmOffset, size_t size)
{
    if (!wasm_runtime_validate_app_addr(moduleInstance, wasmOffset, size)) {
        SPDLOG_ERROR("WASM offset outside WAMR module instance memory"
                     "(offset: {}, size: {})",
                     wasmOffset,
                     size);
        throw std::runtime_error("Offset outside WAMR's memory");
    }
}

uint8_t* WAMRWasmModule::wasmPointerToNative(uint32_t wasmPtr)
{
    void* nativePtr = wasm_runtime_addr_app_to_native(moduleInstance, wasmPtr);
    if (nativePtr == nullptr) {
        SPDLOG_ERROR("WASM offset {} is out of the WAMR module's address space",
                     wasmPtr);
        throw std::runtime_error("Offset out of WAMR memory");
    }
    return static_cast<uint8_t*>(nativePtr);
}

bool WAMRWasmModule::doGrowMemory(uint32_t pageChange)
{
    auto* aotModule = reinterpret_cast<AOTModuleInstance*>(moduleInstance);
    return aot_enlarge_memory(aotModule, pageChange);
}

size_t WAMRWasmModule::getMemorySizeBytes()
{
    auto* aotModule = reinterpret_cast<AOTModuleInstance*>(moduleInstance);
    AOTMemoryInstance* aotMem = ((AOTMemoryInstance**)aotModule->memories)[0];
    return aotMem->cur_page_count * WASM_BYTES_PER_PAGE;
}

uint8_t* WAMRWasmModule::getMemoryBase()
{
    auto* aotModule = reinterpret_cast<AOTModuleInstance*>(moduleInstance);
    AOTMemoryInstance* aotMem = ((AOTMemoryInstance**)aotModule->memories)[0];
    return reinterpret_cast<uint8_t*>(aotMem->memory_data);
}

size_t WAMRWasmModule::getMaxMemoryPages()
{
    auto* aotModule = reinterpret_cast<AOTModuleInstance*>(moduleInstance);
    AOTMemoryInstance* aotMem = ((AOTMemoryInstance**)aotModule->memories)[0];
    return aotMem->max_page_count;
}

WASMModuleInstanceCommon* WAMRWasmModule::getModuleInstance()
{
    if (moduleInstance == nullptr) {
        SPDLOG_ERROR("Trying to get a null-pointing module instance");
        throw std::runtime_error("Null WAMR module instance");
    }
    return moduleInstance;
}

std::vector<std::string> WAMRWasmModule::getArgv()
{
    return argv;
}

uint32_t WAMRWasmModule::mmapFile(uint32_t fp, size_t length)
{
    // TODO - implement
    return 0;
}
}
