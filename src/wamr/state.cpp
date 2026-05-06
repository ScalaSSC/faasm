#include <faabric/executor/ExecutorContext.h>
#include <faabric/planner/PlannerClient.h>
#include <faabric/proto/faabric.pb.h>
#include <faabric/util/logging.h>
#include <faabric/util/serialization.h>

#include <wamr/WAMRWasmModule.h>
#include <wamr/native.h>
#include <wasm/WasmExecutionContext.h>
#include <wasm/WasmModule.h>

#include <wasm_export.h>

#define STREAM_BATCH -2

using namespace faabric::executor;

#define GET_USER_FUNC_PAR()                                                    \
    std::string user;                                                          \
    std::string func;                                                          \
    int32_t parallelismId;                                                     \
    if (ExecutorContext::get()->getMsgIdx() == STREAM_BATCH) {                 \
        SPDLOG_TRACE("S - STREAM_BATCH");                                      \
        faabric::BatchExecuteRequest& req =                                    \
          ExecutorContext::get()->getBatch();                                  \
        user = req.messages(0).user();                                         \
        func = req.messages(0).function();                                     \
        parallelismId = req.messages(0).parallelismid();                       \
    } else {                                                                   \
        user = ExecutorContext::get()->getMsg().user();                        \
        func = ExecutorContext::get()->getMsg().function();                    \
        parallelismId = ExecutorContext::get()->getMsg().parallelismid();      \
    }

namespace wasm {
constexpr uint64_t MAX_PAYLOAD = uint64_t(UINT32_MAX) - sizeof(uint32_t);

/**
 * Read state for the given key into the buffer provided.
 *
 * Returns size of the state if buffer length is zero.
 */
static int32_t __faasm_read_state_wrapper(wasm_exec_env_t exec_env,
                                          char* key,
                                          char* buffer,
                                          int32_t bufferLen)
{
    SPDLOG_DEBUG("S - faasm_read_state {} <buffer> {}", key, bufferLen);

    std::string user = ExecutorContext::get()->getMsg().user();

    if (bufferLen == 0) {
        // If buffer len is zero, just need the state size
        faabric::state::State& state = faabric::state::getGlobalState();
        return (int32_t)state.getStateSize(user, key);
    } else {
        // Write state to buffer
        auto kv = faabric::state::getGlobalState().getKV(user, key, bufferLen);
        kv->get(reinterpret_cast<uint8_t*>(buffer));

        return kv->size();
    }

    return 0;
}

/**
 * Create a new memory region, read the state for the given key into it,
 * then return a pointer to the new memory.
 */
static int32_t __faasm_read_state_ptr_wrapper(wasm_exec_env_t exec_env,
                                              char* key,
                                              int32_t bufferLen)
{
    std::string user = ExecutorContext::get()->getMsg().user();
    auto kv = faabric::state::getGlobalState().getKV(user, key, bufferLen);

    SPDLOG_DEBUG("S - faasm_read_state_ptr - {} {}", kv->key, bufferLen);

    // Map shared memory
    WasmModule* module = getExecutingModule();
    uint32_t wasmPtr = module->mapSharedStateMemory(kv, 0, bufferLen);

    // Call get to make sure the value is pulled
    kv->get();

    return wasmPtr;
}

/**
 * Writes the given data buffer to the state referenced by the given key.
 */
static void __faasm_write_state_wrapper(wasm_exec_env_t exec_env,
                                        char* key,
                                        char* buffer,
                                        int32_t bufferLen)
{
    std::string user = ExecutorContext::get()->getMsg().user();
    auto kv = faabric::state::getGlobalState().getKV(user, key, bufferLen);

    SPDLOG_DEBUG("S - faasm_write_state - {} <data> {}", kv->key, bufferLen);

    kv->set(reinterpret_cast<uint8_t*>(buffer));
}

/**
 * Pushes the state for the given key
 */
static void __faasm_push_state_wrapper(wasm_exec_env_t exec_env, char* key)
{
    SPDLOG_DEBUG("S - faasm_push_state - {}", key);

    std::string user = ExecutorContext::get()->getMsg().user();
    auto kv = faabric::state::getGlobalState().getKV(user, key, 0);
    kv->pushFull();
}

static int32_t __faasm_read_function_state_lock_ptr_wrapper(
  wasm_exec_env_t exec_env,
  int32_t lock)
{
    GET_USER_FUNC_PAR();
    SPDLOG_DEBUG("S - read_function_state_lock_ptr - {}/{}-{} (lock={})",
                 user,
                 func,
                 parallelismId,
                 lock);
    // If the size is 0, it means the function state is not initialized.

    bool dataLock = lock == 1;

    auto stateVec = faabric::state::getGlobalState().readFuncStateLock(
      user, func, parallelismId, dataLock);

    uint64_t dataLen64 = stateVec.size();
    if (dataLen64 == 0) {
        // no state → return 0
        return 0;
    }
    if (dataLen64 > MAX_PAYLOAD) {
        SPDLOG_ERROR(
          "Function state {} bytes exceeds max {}", dataLen64, MAX_PAYLOAD);
        throw std::length_error("Function state exceeds payload limit");
    }

    // total = 4 bytes length prefix + payload
    uint32_t dataLen = static_cast<uint32_t>(dataLen64);
    uint32_t totalLen = dataLen + sizeof(uint32_t);

    // allocate in Wasm linear memory
    WASMModuleInstanceCommon* module_inst =
      wasm_runtime_get_module_inst(exec_env);
    void* native_ptr = nullptr;
    uint32_t app_offset =
      wasm_runtime_module_malloc(module_inst, totalLen, &native_ptr);
    if (app_offset == 0) {
        SPDLOG_ERROR("WASM malloc failed for {} bytes", totalLen);
        throw std::runtime_error("WASM memory allocation failed");
    }

    // write big-endian length prefix
    uint8_t* p = static_cast<uint8_t*>(native_ptr);
    p[0] = uint8_t(dataLen >> 24);
    p[1] = uint8_t(dataLen >> 16);
    p[2] = uint8_t(dataLen >> 8);
    p[3] = uint8_t(dataLen);

    // copy the payload immediately after
    memcpy(p + 4, stateVec.data(), dataLen);

    // return the module‐pointer (offset) back to wasm
    return app_offset;
}

/**
 * Writes the given data buffer to the function state referenced by the given
 * key.
 */
static void __faasm_write_function_state_wrapper(wasm_exec_env_t exec_env,
                                                 char* buffer,
                                                 int32_t bufferLen)
{
    GET_USER_FUNC_PAR();
    SPDLOG_DEBUG(
      "S - faasm_write_function_state - {}/{}-{}", user, func, parallelismId);
    // Create and set the data
    faabric::state::getGlobalState().setFuncState(
      user, func, parallelismId, buffer, bufferLen);
}

/**
 * Writes the given data buffer to the function state referenced by the given
 * key and Unlock the function state.
 */
static void __faasm_write_function_state_unlock_wrapper(
  wasm_exec_env_t exec_env,
  char* buffer,
  int32_t bufferLen)
{
    GET_USER_FUNC_PAR();
    SPDLOG_DEBUG("S - faasm_write_function_state_unlock - {}/{}-{}",
                 user,
                 func,
                 parallelismId);
    // Create and set the data
    faabric::state::getGlobalState().setFuncState(
      user, func, parallelismId, buffer, bufferLen, true);
}

// Function to split a string by a delimiter and store the elements in a set
std::set<std::string> splitStringToSet(const std::string& str,
                                       const std::string& delimiter)
{
    std::set<std::string> resultSet;
    std::size_t start = 0;
    std::size_t end;
    std::size_t delimiter_length = delimiter.length();

    while ((end = str.find(delimiter, start)) != std::string::npos) {
        std::string token = str.substr(start, end - start);
        if (!token.empty()) {
            resultSet.insert(token);
        }
        start = end + delimiter_length;
    }

    // Add the last token if it's not empty
    std::string token = str.substr(start);
    if (!token.empty()) {
        resultSet.insert(token);
    }

    return resultSet;
}

static int32_t __faasm_read_indiv_function_state_ptr_wrapper(
  wasm_exec_env_t exec_env,
  const char* inputKeys)
{
    GET_USER_FUNC_PAR();
    auto& state = faabric::state::getGlobalState();

    std::set<std::string> keys = splitStringToSet(std::string(inputKeys), "|");

    auto lockedStateMap =
      state.readIndivFuncStateLock(user, func, parallelismId, keys);

    // serialize
    auto serializedMap = faabric::util::serializeFuncState(lockedStateMap);
    uint64_t dataLen64 = serializedMap.size();

    if (dataLen64 > MAX_PAYLOAD) {
        SPDLOG_ERROR(
          "Cannot serialize indiv func state: {} bytes exceeds max {}",
          dataLen64,
          MAX_PAYLOAD);
        throw std::length_error("Serialized state exceeds 4 GB limit");
    }

    uint32_t dataLen = static_cast<uint32_t>(dataLen64);
    uint32_t totalLen = dataLen + sizeof(uint32_t);

    // allocate
    WASMModuleInstanceCommon* module_inst =
      wasm_runtime_get_module_inst(exec_env);
    void* native_ptr = nullptr;
    uint32_t app_offset =
      wasm_runtime_module_malloc(module_inst, totalLen, &native_ptr);
    if (app_offset == 0) {
        SPDLOG_ERROR("WASM malloc failed for {} bytes", totalLen);
        throw std::runtime_error("WASM memory allocation failed");
    }

    // write length prefix + payload…
    uint8_t* p = static_cast<uint8_t*>(native_ptr);
    p[0] = uint8_t(dataLen >> 24);
    p[1] = uint8_t(dataLen >> 16);
    p[2] = uint8_t(dataLen >> 8);
    p[3] = uint8_t(dataLen);
    memcpy(p + 4, serializedMap.data(), dataLen);

    return app_offset;
}

static void __faasm_write_indiv_function_state_unlock_wrapper(
  wasm_exec_env_t exec_env,
  char* buffer,
  int32_t bufferLen)
{
    GET_USER_FUNC_PAR();

    faabric::state::State& state = faabric::state::getGlobalState();
    // Split input keys from string into set
    std::vector<uint8_t> bufferVec(buffer, buffer + bufferLen);
    state.writeIndivFuncStateUnlock(user, func, parallelismId, bufferVec);
}

static int32_t __faasm_read_persistent_state_wrapper(wasm_exec_env_t exec_env,
                                                     char* key)
{
    std::string keyData(key);
    SPDLOG_DEBUG("S - faasm_read_persistent_state - key {}", keyData);

    std::string value =
      faabric::state::getGlobalState().readPersistentState(keyData);
    uint32_t str_len = value.size() + 1;

    WASMModuleInstanceCommon* module_inst =
      wasm_runtime_get_module_inst(exec_env);
    void* native_ptr = nullptr;
    uint32_t app_offset =
      wasm_runtime_module_malloc(module_inst, str_len, &native_ptr);
    if (app_offset == 0) {
        SPDLOG_ERROR("Failed to allocate memory for persistent state");
        return 0;
    }

    memcpy(native_ptr, value.c_str(), str_len);
    return app_offset;
}

static void __faasm_write_persistent_state_wrapper(wasm_exec_env_t exec_env,
                                                   char* key,
                                                   char* value)
{
    std::string keyData(key);
    std::string valueData(value);
    SPDLOG_DEBUG("S - faasm_write_persistent_state - key {} and value {}",
                 keyData,
                 valueData);
    faabric::state::getGlobalState().writePersistentState(keyData, valueData);
}

static int32_t __faasm_read_persistent_state_remote_wrapper(
  wasm_exec_env_t exec_env,
  char* key)
{
    std::string keyData(key);
    SPDLOG_DEBUG("S - faasm_read_persistent_state remote - key {}", keyData);

    std::string value =
      faabric::state::getGlobalState().readPersistentStateRemote(keyData);
    uint32_t str_len = value.size() + 1;

    WASMModuleInstanceCommon* module_inst =
      wasm_runtime_get_module_inst(exec_env);
    void* native_ptr = nullptr;
    uint32_t app_offset =
      wasm_runtime_module_malloc(module_inst, str_len, &native_ptr);
    if (app_offset == 0) {
        SPDLOG_ERROR("Failed to allocate memory for persistent state");
        return 0;
    }

    memcpy(native_ptr, value.c_str(), str_len);
    return app_offset;
}

static void __faasm_write_persistent_state_remote_wrapper(
  wasm_exec_env_t exec_env,
  char* key,
  char* value)
{
    std::string keyData(key);
    std::string valueData(value);
    SPDLOG_DEBUG(
      "S - faasm_write_persistent_state remote - key {} and value {}",
      keyData,
      valueData);
    faabric::state::getGlobalState().writePersistentStateRemote(keyData,
                                                                valueData);
}

static NativeSymbol ns[] = {
    REG_NATIVE_FUNC(__faasm_read_state, "($$i)i"),
    REG_NATIVE_FUNC(__faasm_read_state_ptr, "($i)i"),
    REG_NATIVE_FUNC(__faasm_write_state, "($$i)"),
    REG_NATIVE_FUNC(__faasm_push_state, "($)"),
    // The following functions are designed for Function State
    // REG_NATIVE_FUNC(__faasm_read_function_state_size, "(i)i"),
    // REG_NATIVE_FUNC(__faasm_read_function_state, "($i)i"),
    REG_NATIVE_FUNC(__faasm_read_function_state_lock_ptr, "(i)i"),
    REG_NATIVE_FUNC(__faasm_write_function_state, "($i)"),
    REG_NATIVE_FUNC(__faasm_write_function_state_unlock, "($i)"),
    REG_NATIVE_FUNC(__faasm_read_indiv_function_state_ptr, "($)i"),
    REG_NATIVE_FUNC(__faasm_write_indiv_function_state_unlock, "($i)"),
    // The following functions are designed for Persistent State
    REG_NATIVE_FUNC(__faasm_read_persistent_state, "($)i"),
    REG_NATIVE_FUNC(__faasm_write_persistent_state, "($$)"),
    REG_NATIVE_FUNC(__faasm_read_persistent_state_remote, "($)i"),
    REG_NATIVE_FUNC(__faasm_write_persistent_state_remote, "($$)"),
};

uint32_t getFaasmStateApi(NativeSymbol** nativeSymbols)
{
    *nativeSymbols = ns;
    return sizeof(ns) / sizeof(NativeSymbol);
}
}
