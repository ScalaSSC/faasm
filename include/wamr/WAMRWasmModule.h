#pragma once

#include <wamr/WAMRModuleMixin.h>
#include <wasm/WasmModule.h>

#include <wasm_runtime_common.h>

#include <setjmp.h>

#define ERROR_BUFFER_SIZE 256
#define STACK_SIZE_KB 8192
#define HEAP_SIZE_KB 8192

#define WAMR_INTERNAL_EXCEPTION_PREFIX "Exception: "
#define WAMR_EXIT_PREFIX "wamr_exit_code_"

namespace wasm {

class WAMRWasmModule;

#define WAMR_NATIVE_SYMBOL_NOT_IMPLEMENTED(str)                                \
    auto __errorStr = fmt::format("{} not implemented!", str);                 \
    SPDLOG_ERROR(__errorStr);                                                  \
    auto __ex = std::runtime_error(__errorStr);                                \
    auto __module = wasm::getExecutingWAMRModule();                            \
    __module->doThrowException(__ex);

enum WAMRExceptionTypes
{
    NoException = 0,
    DefaultException = 1,
    FunctionMigratedException = 2,
    QueueTimeoutException = 3,
};

std::vector<uint8_t> wamrCodegen(std::vector<uint8_t>& wasmBytesIn, bool isSgx);

class WAMRWasmModule final
  : public WasmModule
  , public WAMRModuleMixin<WAMRWasmModule>
{
  public:
    static void initialiseWAMRGlobally();

    static void destroyWAMRGlobally();

    WAMRWasmModule();

    explicit WAMRWasmModule(int threadPoolSizeIn);

    ~WAMRWasmModule();

    // ----- Module lifecycle -----
    void reset(faabric::Message& msg, const std::string& snapshotKey) override;

    void doBindToFunction(faabric::Message& msg, bool cache) override;

    int32_t executeFunction(faabric::Message& msg) override;

    int32_t executeBatchFunction(faabric::BatchExecuteRequest& req) override;

    // ----- Threads ------
    int32_t executeOMPThread(int threadPoolIdx,
                             uint32_t stackTop,
                             faabric::Message& msg) override;

    int32_t executePthread(int threadPoolIdx,
                           uint32_t stackTop,
                           faabric::Message& msg) override;

    void createThreadsExecEnv(WASMExecEnv* parentExecEnv);

    void destroyThreadsExecEnv(bool destroyMainExecEnv = false);

    // ----- Exception handling -----
    void doThrowException(std::exception& e) override;

    // ----- Helper functions -----
    void writeStringToWasmMemory(const std::string& strHost, char* strWasm);

    void writeWasmEnvToWamrMemory(uint32_t* envOffsetsWasm, char* envBuffWasm);

    // ----- Address translation and validation -----
    // Check if native pointer belongs to WASM memory
    void validateNativePointer(void* nativePtr, int size);

    // Check if WASM offset belongs to WASM memory
    void validateWasmOffset(uint32_t wasmOffset, size_t size);

    // Convert relative address to absolute address (pointer to memory)
    uint8_t* wasmPointerToNative(uint32_t wasmPtr) override;

    // ----- Memory management -----
    uint32_t mmapFile(uint32_t fp, size_t length) override;

    size_t getMemorySizeBytes() override;

    uint8_t* getMemoryBase() override;

    size_t getMaxMemoryPages() override;

    WASMModuleInstanceCommon* getModuleInstance();

    std::vector<std::string> getArgv();

  private:
    bool filesystemPrepared = false;

    char errorBuffer[ERROR_BUFFER_SIZE];

    std::vector<uint8_t> wasmBytes;
    WASMModuleCommon* wasmModule;
    WASMModuleInstanceCommon* moduleInstance;
    // WAMR's execution environments are not thread-safe. Thus, we create an
    // array of them at the beginning, each thread will access a different
    // position in the array, so we do not need a mutex
    std::vector<WASMExecEnv*> execEnvs;

    jmp_buf wamrExceptionJmpBuf;

    int executeWasmFunction(int threadPoolIdx, const std::string& funcName);

    int executeWasmFunctionFromPointer(int threadPoolIdx,
                                       faabric::Message& msg);

    bool executeCatchException(int threadPoolIdx,
                               WASMFunctionInstanceCommon* func,
                               int wasmFuncPtr,
                               int argc,
                               std::vector<uint32_t>& argv);

    void bindInternal(faabric::Message& msg);

    bool doGrowMemory(uint32_t pageChange) override;

    // Light reset (faabric::executor::lightResetEnabled): the instance as it
    // was right after instantiation -- the non-zero pages of linear memory
    // (everything else was zero), memory size, global data and tables -- so
    // that a reset can put it back in place instead of re-instantiating the
    // module. Taken once, on the first instantiation with light reset on:
    // every instance of the module starts out the same.
    struct CleanImage
    {
        // A run of non-zero pages: where it starts in linear memory, how
        // long it is, and where its bytes start in `data`.
        struct Run
        {
            uint64_t offset = 0;
            uint64_t length = 0;
            uint64_t dataOffset = 0;
        };

        bool valid = false;
        std::vector<Run> runs;
        std::vector<uint8_t> data;
        uint64_t memorySize = 0;
        uint32_t pageCount = 0;
        uint32_t bytesPerPage = 0;
        uint32_t maxPageCount = 0;
        std::vector<uint8_t> globals;
        std::vector<std::vector<uint32_t>> tables;
    };
    CleanImage cleanImage;

    void captureCleanImage();

    // Puts the instance back as captured -- a memory that grew shrunk back
    // to its first size -- or returns false, having changed nothing that
    // matters, if it cannot: no image, a table that changed size, or a run
    // that ended in an exception other than a normal exit.
    bool tryLightReset();

    // Makes [start, start + length) of linear memory zero again (light
    // reset): pages the run wrote are zeroed in place and kept, up to
    // `keepBudget` bytes over the whole reset; past that they are dropped.
    // Adds to the byte counts what it zeroed and dropped. False if a drop
    // failed.
    bool zeroLinearRange(uint8_t* start,
                         uint64_t length,
                         uint64_t& keepBudget,
                         uint64_t& zeroedBytes,
                         uint64_t& droppedBytes);

    // Scratch for the pagemap entries read in zeroLinearRange.
    std::vector<uint64_t> pagemapEntries;
};

WAMRWasmModule* getExecutingWAMRModule();
}
