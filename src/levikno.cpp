#include "levikno.h"
#include "levikno_internal.h"

#include <ctime>

#include "stb_image.h"
#include "stb_image_write.h"
#include "miniaudio.h"
#include "freetype/freetype.h"
#include "enet/enet.h"

#ifdef LVN_PLATFORM_WINDOWS
    #include <windows.h>
#endif

#define LVN_ABORT throw std::bad_alloc{};
#define LVN_EMPTY_STR "\0"
#define LVN_DEFAULT_LOG_PATTERN "[%Y-%m-%d] [%T] [%#%l%^] %n: %v%$"

#include "lvn_glfw.h"
#include "lvn_opengl.h"

#if defined(LVN_GRAPHICS_API_INCLUDE_VULKAN)
    #include "lvn_vulkan.h"
#endif

#include "lvn_loaders.h"
#include "lvn_renderer.h"

static LvnContext* s_LvnContext = nullptr;


// ------------------------------------------------------------
// [SECTION]: Audio Internal structs
// ------------------------------------------------------------

struct LvnSound
{
    float volume;
    float pan;
    float pitch;
    bool looping;

    LvnVec3 pos;

    ma_sound sound;
};


// ------------------------------------------------------------
// [SECTION]: Network Internal structs
// ------------------------------------------------------------

struct LvnSocket
{
    LvnSocketType type;

    ENetHost* socket;
    ENetPeer* connection;
    ENetPacket* packet;

    LvnAddress address;
    uint32_t channelCount;
    uint32_t connectionCount;
    uint32_t inBandWidth;
    uint32_t outBandWidth;
};


namespace lvn
{

// memory allocation functions
static void*   mallocWrapper(size_t size, void* userData)               { (void)userData; return malloc(size); }
static void    freeWrapper(void* ptr, void* userData)                   { (void)userData; free(ptr); }
static void*   reallocWrapper(void* ptr, size_t size, void* userData)   { (void)userData; return realloc(ptr, size); }
static LvnMemAllocFunc    s_MemAllocFunc = mallocWrapper;
static LvnMemFreeFunc     s_MemFreeFunc = freeWrapper;
static LvnMemReallocFunc  s_MemReallocFunc = reallocWrapper;
static void*              s_MemAllocUserData = nullptr;


static LvnResult                    initLogging(LvnContextCreateInfo* createInfo);
static void                         terminateLogging();
static LvnVector<LvnLogPattern>     logParseFormat(const char* fmt);
static const char*                  getLogLevelColor(LvnLogLevel level);
static const char*                  getLogLevelName(LvnLogLevel level);
static const char*                  getWindowApiNameEnum(LvnWindowApi api);
static const char*                  getGraphicsApiNameEnum(LvnGraphicsApi api);
static LvnResult                    setWindowContext(LvnContext* lvnctx, LvnWindowApi windowapi);
static void                         terminateWindowContext(LvnContext* lvnctx);
static LvnResult                    setGraphicsContext(LvnContext* lvnctx, LvnGraphicsApi graphicsapi);
static void                         terminateGraphicsContext(LvnContext* lvnctx);
static LvnResult                    initAudioContext(LvnContext* lvnctx);
static void                         terminateAudioContext(LvnContext* lvnctx);
static LvnResult                    initNetworkingContext();
static void                         terminateNetworkingContext();
static void                         initStandardPipelineSpecification(LvnContext* lvnctx);
static void                         setDefaultStructTypeMemAllocInfos(LvnContext* lvnctx);
static const char*                  getStructTypeEnumStr(LvnStructureType stype);
static uint64_t                     getStructTypeSize(LvnStructureType sType);
static LvnData<uint32_t>            initDefaultFontCodepoints();
static LvnResult                    createContextMemoryPool(LvnContext* lvnctx, LvnContextCreateInfo* createInfo);
static void                         createMemoryBlock(LvnContext* lvnctx, LvnStructureType sType);

template <typename T>
static T* createObject(LvnContext* lvnctx, LvnStructureType sType);

template <typename T>
static void destroyObject(LvnContext* lvnctx, T* obj, LvnStructureType sType);


// Windows platform specific; enables console output colors
#ifdef LVN_PLATFORM_WINDOWS
static void enableLogANSIcodeColors()
{
    DWORD consoleMode;
    HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(outputHandle, &consoleMode))
    {
        SetConsoleMode(outputHandle, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}
#endif

static const char* getLogLevelColor(LvnLogLevel level)
{
    switch (level)
    {
        case Lvn_LogLevel_None:     { return LVN_LOG_COLOR_RESET; }
        case Lvn_LogLevel_Trace:    { return LVN_LOG_COLOR_TRACE; }
        case Lvn_LogLevel_Debug:    { return LVN_LOG_COLOR_DEBUG; }
        case Lvn_LogLevel_Info:     { return LVN_LOG_COLOR_INFO; }
        case Lvn_LogLevel_Warn:     { return LVN_LOG_COLOR_WARN; }
        case Lvn_LogLevel_Error:    { return LVN_LOG_COLOR_ERROR; }
        case Lvn_LogLevel_Fatal:    { return LVN_LOG_COLOR_FATAL; }
    }

    return nullptr;
}

static const char* getLogLevelName(LvnLogLevel level)
{
    switch (level)
    {
        case Lvn_LogLevel_None:     { return "none"; }
        case Lvn_LogLevel_Trace:    { return "trace"; }
        case Lvn_LogLevel_Debug:    { return "debug"; }
        case Lvn_LogLevel_Info:     { return "info"; }
        case Lvn_LogLevel_Warn:     { return "warn"; }
        case Lvn_LogLevel_Error:    { return "error"; }
        case Lvn_LogLevel_Fatal:    { return "fatal"; }
    }

    return nullptr;
}

static const char* getWindowApiNameEnum(LvnWindowApi api)
{
    switch (api)
    {
        case Lvn_WindowApi_None:  { return "None";  }
        case Lvn_WindowApi_glfw:  { return "glfw";  }
        // case Lvn_WindowApi_win32: { return "win32"; }
    }

    return LVN_EMPTY_STR;
}

static const char* getGraphicsApiNameEnum(LvnGraphicsApi api)
{
    switch (api)
    {
        case Lvn_GraphicsApi_None:   { return "None";   }
        case Lvn_GraphicsApi_vulkan: { return "vulkan"; }
        case Lvn_GraphicsApi_opengl: { return "opengl"; }
    }

    return LVN_EMPTY_STR;
}

static LvnResult setWindowContext(LvnContext* lvnctx, LvnWindowApi windowapi)
{
    LvnResult result = Lvn_Result_Failure;
    switch (windowapi)
    {
        case Lvn_WindowApi_None:
        {
            LVN_CORE_TRACE("no window context selected; window related function calls will not be used");
            return Lvn_Result_Success;
        }
        case Lvn_WindowApi_glfw:
        {
            result = glfwImplInitWindowContext(&lvnctx->windowContext);
            break;
        }
        // case Lvn_WindowApi_win32:
        // {
        //  break;
        // }
    }

    //windowInputInit();

    if (result != Lvn_Result_Success)
        LVN_CORE_ERROR("could not create window context for: %s", getWindowApiNameEnum(windowapi));
    else
        LVN_CORE_TRACE("window context set: %s", getWindowApiNameEnum(windowapi));

    return result;
}

static void terminateWindowContext(LvnContext* lvnctx)
{
    switch (lvnctx->windowapi)
    {
        case Lvn_WindowApi_None:
        {
            LVN_CORE_TRACE("no window api selected, no window context to terminate");
            return;
        }
        case Lvn_WindowApi_glfw:
        {
            glfwImplTerminateWindowContext();
            break;
        }
        // case Lvn_WindowApi_win32:
        // {
        //  break;
        // }
        default:
        {
            LVN_CORE_ERROR("unknown windows api selected, cannot terminate window context");
            return;
        }
    }

    LVN_CORE_TRACE("window context terminated: %s", getWindowApiNameEnum(lvnctx->windowapi));
}

static LvnResult setGraphicsContext(LvnContext* lvnctx, LvnGraphicsApi graphicsapi)
{
    LvnResult result = Lvn_Result_Failure;
    switch (graphicsapi)
    {
        case Lvn_GraphicsApi_None:
        {
            LVN_CORE_TRACE("no graphics context selected; graphics related function calls will not be used");
            return Lvn_Result_Success;
        }
        case Lvn_GraphicsApi_vulkan:
        {
        #if defined(LVN_GRAPHICS_API_INCLUDE_VULKAN)
            result = vksImplCreateContext(&lvnctx->graphicsContext);
        #endif
            break;
        }
        case Lvn_GraphicsApi_opengl:
        {
            result = oglsImplCreateContext(&lvnctx->graphicsContext);
            break;
        }
    }

    if (result != Lvn_Result_Success)
        LVN_CORE_ERROR("could not create graphics context for: %s", getGraphicsApiNameEnum(graphicsapi));
    else
        LVN_CORE_TRACE("graphics context set: %s", getGraphicsApiNameEnum(graphicsapi));

    return result;
}

static void terminateGraphicsContext(LvnContext* lvnctx)
{
    switch (lvnctx->graphicsapi)
    {
        case Lvn_GraphicsApi_None:
        {
            LVN_CORE_TRACE("no graphics api selected, no graphics context to terminate");
            return;
        }
        case Lvn_GraphicsApi_vulkan:
        {
        #if defined(LVN_GRAPHICS_API_INCLUDE_VULKAN)
            vksImplTerminateContext();
        #endif
            break;
        }
        case Lvn_GraphicsApi_opengl:
        {
            oglsImplTerminateContext();
            break;
        }
        default:
        {
            LVN_CORE_ERROR("unknown graphics api selected, cannot terminate graphics context");
        }
    }

    LVN_CORE_TRACE("graphics context terminated: %s", getGraphicsApiNameEnum(lvnctx->graphicsapi));
}

static LvnResult initAudioContext(LvnContext* lvnctx)
{
    ma_engine* pEngine = (ma_engine*)LVN_MALLOC(sizeof(ma_engine));

    if (ma_engine_init(nullptr, pEngine) != MA_SUCCESS)
    {
        LVN_CORE_ERROR("failed to initialize audio engine context");
        return Lvn_Result_Failure;
    }

    lvnctx->audioEngineContextPtr = pEngine;

    LVN_CORE_TRACE("audio context initialized");
    return Lvn_Result_Success;
}

static void terminateAudioContext(LvnContext* lvnctx)
{
    if (lvnctx->audioEngineContextPtr != nullptr)
    {
        ma_engine_uninit(static_cast<ma_engine*>(lvnctx->audioEngineContextPtr));
        lvn::memFree(lvnctx->audioEngineContextPtr);
    }

    LVN_CORE_TRACE("audio context terminated");
}

static LvnResult initNetworkingContext()
{
    if (enet_initialize() != 0)
    {
        LVN_CORE_ERROR("failed to initialize networking context");
        return Lvn_Result_Failure;
    }

    LVN_CORE_TRACE("networking context initialized");
    return Lvn_Result_Success;
}

static void terminateNetworkingContext()
{
    enet_deinitialize();
    LVN_CORE_TRACE("networking context terminated");
}

static void initStandardPipelineSpecification(LvnContext* lvnctx)
{
    LvnPipelineSpecification pipelineSpecification{};

    // Input Assembly
    pipelineSpecification.inputAssembly.topology = Lvn_TopologyType_Triangle;
    pipelineSpecification.inputAssembly.primitiveRestartEnable = false;

    // Viewport
    pipelineSpecification.viewport.x = 0.0f;
    pipelineSpecification.viewport.y = 0.0f;
    pipelineSpecification.viewport.width = 800.0f;
    pipelineSpecification.viewport.height = 600.0f;
    pipelineSpecification.viewport.minDepth = 0.0f;
    pipelineSpecification.viewport.maxDepth = 1.0f;

    // Scissor
    pipelineSpecification.scissor.offset = { 0, 0 };
    pipelineSpecification.scissor.extent = { 800, 600 };

    // Rasterizer
    pipelineSpecification.rasterizer.depthClampEnable = false;
    pipelineSpecification.rasterizer.rasterizerDiscardEnable = false;
    pipelineSpecification.rasterizer.lineWidth = 1.0f;
    pipelineSpecification.rasterizer.cullMode = Lvn_CullFaceMode_Disable;
    pipelineSpecification.rasterizer.frontFace = Lvn_CullFrontFace_Clockwise;
    pipelineSpecification.rasterizer.depthBiasEnable = false;
    pipelineSpecification.rasterizer.depthBiasConstantFactor = 0.0f;
    pipelineSpecification.rasterizer.depthBiasClamp = 0.0f;
    pipelineSpecification.rasterizer.depthBiasSlopeFactor = 0.0f;

    // MultiSampling
    pipelineSpecification.multisampling.sampleShadingEnable = false;
    pipelineSpecification.multisampling.rasterizationSamples = Lvn_SampleCount_1_Bit;
    pipelineSpecification.multisampling.minSampleShading = 1.0f;
    pipelineSpecification.multisampling.sampleMask = nullptr;
    pipelineSpecification.multisampling.alphaToCoverageEnable = false;
    pipelineSpecification.multisampling.alphaToOneEnable = false;

    // Color Attachments
    pipelineSpecification.colorBlend.colorBlendAttachmentCount = 0; // If no attachments are provided, an attachment will automatically be created
    pipelineSpecification.colorBlend.pColorBlendAttachments = nullptr; 

    // Color Blend
    pipelineSpecification.colorBlend.logicOpEnable = false;
    pipelineSpecification.colorBlend.blendConstants[0] = 0.0f;
    pipelineSpecification.colorBlend.blendConstants[1] = 0.0f;
    pipelineSpecification.colorBlend.blendConstants[2] = 0.0f;
    pipelineSpecification.colorBlend.blendConstants[3] = 0.0f;

    // Depth Stencil
    pipelineSpecification.depthstencil.enableDepth = false;
    pipelineSpecification.depthstencil.depthOpCompare = Lvn_CompareOp_Never;
    pipelineSpecification.depthstencil.enableStencil = false;
    pipelineSpecification.depthstencil.stencil.compareMask = 0x00;
    pipelineSpecification.depthstencil.stencil.writeMask = 0x00;
    pipelineSpecification.depthstencil.stencil.reference = 0;
    pipelineSpecification.depthstencil.stencil.compareOp = Lvn_CompareOp_Never;
    pipelineSpecification.depthstencil.stencil.depthFailOp = Lvn_StencilOp_Keep;
    pipelineSpecification.depthstencil.stencil.failOp = Lvn_StencilOp_Keep;
    pipelineSpecification.depthstencil.stencil.passOp = Lvn_StencilOp_Keep;

    lvnctx->defaultPipelineSpecification = pipelineSpecification;
}

static void setDefaultStructTypeMemAllocInfos(LvnContext* lvnctx)
{
    auto& stInfos = lvnctx->sTypeMemAllocInfos;

    stInfos.resize(Lvn_Stype_Max_Value);

    stInfos[Lvn_Stype_Undefined]        = { Lvn_Stype_Undefined, 0, 0 };
    stInfos[Lvn_Stype_Window]           = { Lvn_Stype_Window, sizeof(LvnWindow), 8 };
    stInfos[Lvn_Stype_Logger]           = { Lvn_Stype_Logger, sizeof(LvnLogger), 8 };
    stInfos[Lvn_Stype_FrameBuffer]      = { Lvn_Stype_FrameBuffer, sizeof(LvnFrameBuffer), 16 };
    stInfos[Lvn_Stype_Shader]           = { Lvn_Stype_Shader, sizeof(LvnShader), 32 };
    stInfos[Lvn_Stype_DescriptorLayout] = { Lvn_Stype_DescriptorLayout, sizeof(LvnDescriptorLayout), 64 };
    stInfos[Lvn_Stype_Pipeline]         = { Lvn_Stype_Pipeline, sizeof(LvnPipeline), 64 };
    stInfos[Lvn_Stype_Buffer]           = { Lvn_Stype_Buffer, sizeof(LvnBuffer), 256 };
    stInfos[Lvn_Stype_Sampler]          = { Lvn_Stype_Sampler, sizeof(LvnSampler), 256 };
    stInfos[Lvn_Stype_Texture]          = { Lvn_Stype_Texture, sizeof(LvnTexture), 256 };
    stInfos[Lvn_Stype_Cubemap]          = { Lvn_Stype_Cubemap, sizeof(LvnCubemap), 256 };
    stInfos[Lvn_Stype_Sound]            = { Lvn_Stype_Sound, sizeof(LvnSound), 32 };
    stInfos[Lvn_Stype_Socket]           = { Lvn_Stype_Socket, sizeof(LvnSocket), 32 };
}

static const char* getStructTypeEnumStr(LvnStructureType stype)
{
    switch (stype)
    {
        case Lvn_Stype_Window:            { return "LvnWindow"; }
        case Lvn_Stype_Logger:            { return "LvnLogger"; }
        case Lvn_Stype_FrameBuffer:       { return "LvnFrameBuffer"; }
        case Lvn_Stype_Shader:            { return "LvnShader"; }
        case Lvn_Stype_DescriptorLayout:  { return "LvnDescriptorLayout"; }
        case Lvn_Stype_Pipeline:          { return "LvnPipeline"; }
        case Lvn_Stype_Buffer:            { return "LvnBuffer"; }
        case Lvn_Stype_Sampler:           { return "LvnSampler"; }
        case Lvn_Stype_Texture:           { return "LvnTexture"; }
        case Lvn_Stype_Cubemap:           { return "LvnCubemap"; }
        case Lvn_Stype_Sound:             { return "LvnSound"; }
        case Lvn_Stype_Socket:            { return "LvnSocket"; }

        default:                          { return "undefined"; }
    }
}

static uint64_t getStructTypeSize(LvnStructureType sType)
{
    return lvn::getContext()->sTypeMemAllocInfos[sType].size;
}

static LvnData<uint32_t> initDefaultFontCodepoints()
{
    LvnVector<uint32_t> codepoints; codepoints.reserve(126-31+191-160);
    for (uint32_t i = 32; i <= 126; i++)
        codepoints.push_back(i);
    for (uint32_t i = 160; i <= 255; i++)
        codepoints.push_back(i);

    return LvnData<uint32_t>(codepoints.data(), codepoints.size());
}

static LvnResult createContextMemoryPool(LvnContext* lvnctx, LvnContextCreateInfo* createInfo)
{
    lvn::setDefaultStructTypeMemAllocInfos(lvnctx);

    lvnctx->memoryMode = createInfo->memoryInfo.memAllocMode;
    if (lvnctx->memoryMode == Lvn_MemAllocMode_Individual) { return Lvn_Result_Success; }

    // set struct memory configs
    lvnctx->blockMemAllocInfos = lvnctx->sTypeMemAllocInfos;
    auto& structTypes = lvnctx->sTypeMemAllocInfos;
    for (uint64_t i = 0; i < createInfo->memoryInfo.memoryBindingCount; i++)
    {
        if (createInfo->memoryInfo.pMemoryBindings[i].count == 0)
        {
            LVN_CORE_ERROR("[context]: createInfo->memoryInfo.pMemoryBindings[%u].count is 0, cannot have a memory binding with a count of 0", i);
            return Lvn_Result_Failure;
        }

        structTypes[createInfo->memoryInfo.pMemoryBindings[i].sType].count = createInfo->memoryInfo.pMemoryBindings[i].count;
    }

    // get total memory in bytes for memory pool
    uint64_t memSize = 0;
    for (uint64_t i = 0; i < structTypes.size(); i++)
        memSize += structTypes[i].size * structTypes[i].count;

    // create the first memory block
    LvnMemoryPool* memPool = &lvnctx->memoryPool;
    memPool->baseMemoryBlock = LvnMemoryBlock(memSize);

    memPool->memBlocks.resize(Lvn_Stype_Max_Value); // future memory blocks
    memPool->memBindings.resize(Lvn_Stype_Max_Value);

    // set memory block bindings (all the first bindings are within the first memory block)
    // newly created memory bindings will have their own indicidual memory blocks
    uint64_t memIndex = 0;
    for (uint32_t i = 0; i < structTypes.size(); i++)
    {
        auto& memBinding = memPool->memBindings[structTypes[i].sType];

        uint64_t count = structTypes[i].count;
        memBinding.push_back(LvnMemoryBinding(memPool->baseMemoryBlock[memIndex], structTypes[i].size, count));
        memIndex += count * structTypes[i].size;
    }

    // set struct block memory configs
    for (uint64_t i = 0; i < createInfo->memoryInfo.blockMemoryBindingCount; i++)
    {
        if (createInfo->memoryInfo.pBlockMemoryBindings[i].count == 0)
        {
            LVN_CORE_ERROR("[context]: createInfo->memoryInfo.pBlockMemoryBindings[%u].count is 0, cannot have a memory binding with a count of 0", i);
            return Lvn_Result_Failure;
        }

        lvnctx->blockMemAllocInfos[createInfo->memoryInfo.pBlockMemoryBindings[i].sType].count = createInfo->memoryInfo.pBlockMemoryBindings[i].count;
    }

    LVN_CORE_TRACE("memory allocation mode set to memory pool, %u custom base memory bindings created, %u custom memory block bindings created, total base memory pool size: %zu bytes",
        createInfo->memoryInfo.memoryBindingCount,
        createInfo->memoryInfo.blockMemoryBindingCount,
        memSize);

    return Lvn_Result_Success;
}

static void createMemoryBlock(LvnContext* lvnctx, LvnStructureType sType)
{
    uint64_t size = lvnctx->blockMemAllocInfos[sType].size;
    uint64_t count = lvnctx->blockMemAllocInfos[sType].count;
    uint64_t memsize = size * count;

    // create the next memory block in the list for sType
    LvnMemoryPool* memPool = &lvnctx->memoryPool;
    memPool->memBlocks[sType].push_back(LvnMemoryBlock(memsize));

    // set memory binding for sType
    auto& memBinding = memPool->memBindings[sType];

    LvnMemoryBinding* prevMemBinding = nullptr;
    if (!memBinding.empty())
        prevMemBinding = &memBinding.back();

    // bind the memory binding for sType to the newly created memory block
    memBinding.push_back(LvnMemoryBinding(memPool->memBlocks[sType].back()[0], size, count));

    // set the previous memory binding to the newly created memory binding
    if (prevMemBinding != nullptr)
        prevMemBinding->set_next_memory_binding(&memBinding.back());
}

template <typename T>
static T* createObject(LvnContext* lvnctx, LvnStructureType sType)
{
    T* object;
    if (lvnctx->memoryMode == Lvn_MemAllocMode_Individual)
    {
        object = new T();
    }
    else if (lvnctx->memoryMode == Lvn_MemAllocMode_MemPool)
    {
        auto& memBinding = lvnctx->memoryPool.memBindings[sType][0];
        if (memBinding.find_empty_memory_binding() == nullptr)
            lvn::createMemoryBlock(lvnctx, sType);

        object = new (static_cast<T*>(memBinding.take_next())) T();
    }
    else
    {
        LVN_CORE_ASSERT(false, "create object failed, no requirment was met before hand"); return nullptr;
    }

    lvnctx->objectMemoryAllocations.sTypes[sType].count++;
    return object;
}

template <typename T>
static void destroyObject(LvnContext* lvnctx, T* obj, LvnStructureType sType)
{
    if (lvnctx->memoryMode == Lvn_MemAllocMode_Individual)
    {
        delete obj;
        obj = nullptr;
    }
    else if (lvnctx->memoryMode == Lvn_MemAllocMode_MemPool)
    {
        lvnctx->memoryPool.memBindings[sType][0].push_back(obj);
    }
    else
    {
        LVN_CORE_ASSERT(false, "destroy object failed, no requirment was met before hand");
    }

    lvnctx->objectMemoryAllocations.sTypes[sType].count--;
}

// ------------------------------------------------------------
// [SECTION]: Core Functions
// ------------------------------------------------------------

LvnResult createContext(LvnContextCreateInfo* createInfo)
{
    if (s_LvnContext != nullptr) { return Lvn_Result_AlreadyCalled; }
    s_LvnContext = new LvnContext();
    LvnContext* lvnctx = s_LvnContext;

    lvnctx->contexTime.reset();

    lvnctx->appName = createInfo->applicationName;
    lvnctx->windowapi = createInfo->windowapi;
    lvnctx->graphicsapi = createInfo->graphicsapi;
    lvnctx->multithreading = createInfo->enableMultithreading;

    lvnctx->graphicsContext.graphicsapi = createInfo->graphicsapi;
    lvnctx->graphicsContext.enableGraphicsApiDebugLogs = createInfo->logging.enableGraphicsApiDebugLogs;
    lvnctx->graphicsContext.frameBufferColorFormat = createInfo->rendering.frameBufferColorFormat;
    lvnctx->graphicsContext.maxFramesInFlight = createInfo->rendering.maxFramesInFlight;

    // logging
    lvn::initLogging(createInfo);

    // memory
    lvnctx->objectMemoryAllocations.sTypes.resize(Lvn_Stype_Max_Value);
    for (uint32_t i = 0; i < lvnctx->objectMemoryAllocations.sTypes.size(); i++)
    {
        lvnctx->objectMemoryAllocations.sTypes[i] = { (LvnStructureType)i, 0 };
    }

    // default font codepoints
    lvnctx->defaultCodePoints = lvn::initDefaultFontCodepoints();

    // memory pool
    LvnResult result = lvn::createContextMemoryPool(lvnctx, createInfo);
    if (result != Lvn_Result_Success) { return result; }

    // window context
    result = setWindowContext(lvnctx, createInfo->windowapi);
    if (result != Lvn_Result_Success) { return result; }

    // graphics context
    result = setGraphicsContext(lvnctx, createInfo->graphicsapi);
    if (result != Lvn_Result_Success) { return result; }

    // audio context
    result = initAudioContext(lvnctx);
    if (result != Lvn_Result_Success) { return result; }

    // networking context
    result = initNetworkingContext();
    if (result != Lvn_Result_Success) { return result; }

    // config
    initStandardPipelineSpecification(lvnctx);

    if (createInfo->rendering.matrixClipRegion == Lvn_ClipRegion_ApiSpecific)
    {
        switch(createInfo->graphicsapi)
        {
            case Lvn_GraphicsApi_opengl:
            {
                lvnctx->matrixClipRegion = Lvn_ClipRegion_RHNO;
                break;
            }
            case Lvn_GraphicsApi_vulkan:
            {
                lvnctx->matrixClipRegion = Lvn_ClipRegion_LHZO;
                break;
            }

            default: { break; }
        }
    }
    else
    {
        lvnctx->matrixClipRegion = createInfo->rendering.matrixClipRegion;
    }

    return Lvn_Result_Success;
}

void terminateContext()
{
    if (s_LvnContext == nullptr) { return; }

    LvnContext* lvnctx = s_LvnContext;

    if (lvn::rendererIsInitialized())
        lvn::renderTerminate();

    lvn::terminateGraphicsContext(lvnctx);
    lvn::terminateWindowContext(lvnctx);
    lvn::terminateAudioContext(lvnctx);
    lvn::terminateNetworkingContext();

    for (uint32_t i = 0; i < lvnctx->objectMemoryAllocations.sTypes.size(); i++)
    {
        if (lvnctx->objectMemoryAllocations.sTypes[i].count > 0)
        {
            const char* stype = lvn::getStructTypeEnumStr(lvnctx->objectMemoryAllocations.sTypes[i].sType);
            LVN_CORE_ERROR("sType = %s | not all objects of this sType (%s) have been destroyed, number of %s objects remaining: %zu", stype, stype, stype, lvnctx->objectMemoryAllocations.sTypes[i].count);
        }
    }

    if (lvnctx->numMemoryAllocations > 0) { LVN_CORE_WARN("not all memory allocations have been freed, number of allocations remaining: %zu", lvnctx->numMemoryAllocations); }

    lvn::terminateLogging();

    delete s_LvnContext;
    s_LvnContext = nullptr;
}

LvnContext* getContext()
{
    LVN_CORE_ASSERT(s_LvnContext != nullptr, "levikno context is nullptr, context was probably not created or initiated before using the library")
    return s_LvnContext;
}

// ------------------------------------------------------------
// [SECTION]: Date Time Functions
// ------------------------------------------------------------

int dateGetYear()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return tm.tm_year + 1900;
}
int dateGetYear02d()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return (tm.tm_year + 1900) % 100;
}
int dateGetMonth()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return tm.tm_mon + 1;
}
int dateGetDay()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return tm.tm_mday;
}
int dateGetHour()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return tm.tm_hour;
}
int dateGetHour12()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return ((tm.tm_hour + 11) % 12) + 1;
}
int dateGetMinute()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return tm.tm_min;
}
int dateGetSecond()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return tm.tm_sec;
}

long long dateGetSecondsSinceEpoch()
{
    return time(NULL);
}

static const char* const s_MonthName[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static const char* const s_MonthNameShort[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char* const s_WeekDayName[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char* const s_WeekDayNameShort[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

const char* dateGetMonthName()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return s_MonthName[tm.tm_mon];
}
const char* dateGetMonthNameShort()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return s_MonthNameShort[tm.tm_mon];
}
const char* dateGetWeekDayName()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return s_WeekDayName[tm.tm_wday];
}
const char* dateGetWeekDayNameShort()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    return s_WeekDayNameShort[tm.tm_wday];
}
const char* dateGetTimeMeridiem()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    if (tm.tm_hour < 12)
        return "AM";
    else
        return "PM";
}
const char* dateGetTimeMeridiemLower()
{
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    if (tm.tm_hour < 12)
        return "am";
    else
        return "pm";
}

LvnString dateGetTimeHHMMSS()
{
    char buff[9];
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    snprintf(buff, 9, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return LvnString(buff);
}
LvnString dateGetTime12HHMMSS()
{
    char buff[9];
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    snprintf(buff, 9, "%02d:%02d:%02d", ((tm.tm_hour + 11) % 12) + 1, tm.tm_min, tm.tm_sec);
    return LvnString(buff);
}
LvnString dateGetYearStr()
{
    char buff[5];
    snprintf(buff, 5, "%d", dateGetYear());
    return LvnString(buff);
}
LvnString dateGetYear02dStr()
{
    char buff[3];
    snprintf(buff, 3, "%d", dateGetYear02d());
    return LvnString(buff);
}
LvnString dateGetMonthNumStr()
{
    char buff[3];
    snprintf(buff, 3, "%02d", dateGetMonth());
    return LvnString(buff);
}
LvnString dateGetDayNumStr()
{
    char buff[3];
    snprintf(buff, 3, "%02d", dateGetDay());
    return LvnString(buff);
}
LvnString dateGetHourNumStr()
{
    char buff[3];
    snprintf(buff, 3, "%02d", dateGetHour());
    return LvnString(buff);
}
LvnString dateGetHour12NumStr()
{
    char buff[3];
    snprintf(buff, 3, "%02d", dateGetHour12());
    return LvnString(buff);
}
LvnString dateGetMinuteNumStr()
{
    char buff[3];
    snprintf(buff, 3, "%02d", dateGetMinute());
    return LvnString(buff);
}
LvnString dateGetSecondNumStr()
{
    char buff[3];
    snprintf(buff, 3, "%02d", dateGetSecond());
    return LvnString(buff);
}


LvnString loadFileSrc(const char* filepath)
{
    FILE* fileptr = fopen(filepath, "r");

    if (!fileptr)
    {
        LVN_CORE_ERROR("cannot open source file: %s", filepath);
        return {};
    }

    fseek(fileptr, 0, SEEK_END);
    long int size = ftell(fileptr);
    fseek(fileptr, 0, SEEK_SET);

    LvnVector<char> src(size);
    fread(src.data(), sizeof(char), size, fileptr);
    fclose(fileptr);

    return LvnString(src.data(), src.size());
}

float getContextTime()
{
    return lvn::getContext()->contexTime.elapsed();
}

LvnData<uint8_t> loadFileSrcBin(const char* filepath)
{
    FILE* fileptr = fopen(filepath, "rb");

    if (!fileptr)
    {
        LVN_CORE_ERROR("cannot open binary file: %s", filepath);
        return {};
    }

    fseek(fileptr, 0, SEEK_END);
    long int size = ftell(fileptr);
    fseek(fileptr, 0, SEEK_SET);

    LvnVector<uint8_t> bin(size);
    fread(bin.data(), sizeof(uint8_t), size, fileptr);
    fclose(fileptr);

    return LvnData<uint8_t>(bin.data(), bin.size());
}

void writeFileSrc(const char* filename, const char* src, LvnFileMode mode)
{
    const char* filemode = "w";
    if (mode == Lvn_FileMode_Write) filemode = "w";
    else if (mode == Lvn_FileMode_Append) filemode = "a";

    FILE* fileptr = fopen(filename, filemode);

    if (!fileptr)
    {
        LVN_CORE_ERROR("cannot write to source file: %s", filename);
        return;
    }

    fprintf(fileptr, "%s", src);
    fclose(fileptr);
}

LvnFont loadFontFromFileTTF(const char* filepath, uint32_t fontSize, const uint32_t* pCodepoints, uint32_t codepointCount, LvnLoadFontFlagBits flags)
{
    LvnFont font{};

    if (pCodepoints == nullptr)
    {
        LvnContext* lvnctx = lvn::getContext();
        pCodepoints = lvnctx->defaultCodePoints.data();
        codepointCount = lvnctx->defaultCodePoints.size();
    }

    FT_Library ft;
    FT_Face face;

    if (FT_Init_FreeType(&ft))
    {
        LVN_CORE_ERROR("[freetype]: failed to load freetype library");
        LVN_CORE_ASSERT(false, "failed to load freetype");
        return font;
    }

    if (FT_New_Face(ft, filepath, 0, &face))
    {
        LVN_CORE_ERROR("[freetype]: failed to load font face!");
        LVN_CORE_ASSERT(false, "failed to load font face");
        return font;
    }

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)fontSize);

    int maxDim = (1 + (face->size->metrics.height >> 6)) * ceilf(sqrtf(codepointCount));
    int width = 1;
    while (width < maxDim) width <<= 1;
    int height = width;

    // render glyphs to atlas
    LvnVector<uint8_t> pixels(width * height);
    int penx = 0, peny = 0;
    const int padding = 2;
    const int lineHeight = (face->size->metrics.height >> 6) + padding;

    LvnVector<LvnFontGlyph> glyphs(codepointCount);

    uint32_t loadFlags = FT_LOAD_RENDER;
    if (flags & Lvn_LoadFont_NoHinting)
        loadFlags |= FT_LOAD_NO_HINTING;
    if (flags & Lvn_LoadFont_AutoHinting)
        loadFlags |= FT_LOAD_FORCE_AUTOHINT;
    if (flags & Lvn_LoadFont_TargetLight)
        loadFlags |= FT_LOAD_TARGET_LIGHT;
    if (flags & Lvn_LoadFont_TargetMono)
        loadFlags |= FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME;

    for (uint32_t i = 0; i < codepointCount; i++)
    {
        FT_Load_Char(face, pCodepoints[i], loadFlags);
        FT_Bitmap* bmp = &face->glyph->bitmap;

        if (penx + bmp->width + padding > width)
        {
            penx = padding;
            peny += lineHeight;
        }

        if (bmp->pixel_mode == FT_PIXEL_MODE_MONO && flags & Lvn_LoadFont_TargetMono)
        {
            for (uint32_t row = 0; row < bmp->rows; ++row)
            {
                for (uint32_t col = 0; col < bmp->width; ++col)
                {
                    int byteIndex = col / 8;
                    int bitIndex = 7 - (col % 8);
                    uint8_t byte = bmp->buffer[row * bmp->pitch + byteIndex];
                    bool bitSet = (byte >> bitIndex) & 1;
                    int x = penx + col;
                    int y = peny + row;
                    if (x < width && y < height)
                        pixels[y * width + x] = bitSet ? 255 : 0;
                }
            }
        }
        else
        {
            for (uint32_t row = 0; row < bmp->rows; row++)
            {
                for (uint32_t col = 0; col < bmp->width; col++)
                {
                    int x = penx + col;
                    int y = peny + row;
                    pixels[y * width + x] = bmp->buffer[row * abs(bmp->pitch) + col];
                }
            }
        }


        LvnFontGlyph glyph{};
        glyph.uv.x0 = (float)penx / (float)width;
        glyph.uv.y0 = (float)peny / (float)height;
        glyph.uv.x1 = (float)(penx + bmp->width) / (float)width;
        glyph.uv.y1 = (float)(peny + bmp->rows) / (float)height;

        glyph.size.x = bmp->width;
        glyph.size.y = bmp->rows;
        glyph.bearing.x = face->glyph->bitmap_left;
        glyph.bearing.y = face->glyph->bitmap_top;
        glyph.advance = face->glyph->advance.x >> 6;
        glyph.unicode = pCodepoints[i];

        glyphs[i] = glyph;

        penx += bmp->width + padding;
    }

    FT_Done_FreeType(ft);

    LvnImageData atlas{};
    atlas.width = width;
    atlas.height = height;
    atlas.channels = 1;
    atlas.size = width * height;
    atlas.pixels = LvnData<uint8_t>(pixels.data(), pixels.size());

    font.atlas = atlas;
    font.glyphs = LvnData<LvnFontGlyph>(glyphs.data(), glyphs.size());
    font.codepoints = LvnData<uint32_t>(pCodepoints, codepointCount);
    font.fontSize = fontSize;

    return font;
}

// TODO: refactor font loading and duplicate code
LvnFont loadFontFromFileTTFMemory(const uint8_t* fontData, uint64_t fontDataSize, uint32_t fontSize, const uint32_t* pCodepoints, uint32_t codepointCount, LvnLoadFontFlagBits flags)
{
    LvnFont font{};

    if (pCodepoints == nullptr)
    {
        LvnContext* lvnctx = lvn::getContext();
        pCodepoints = lvnctx->defaultCodePoints.data();
        codepointCount = lvnctx->defaultCodePoints.size();
    }

    FT_Library ft;
    FT_Face face;

    if (FT_Init_FreeType(&ft))
    {
        LVN_CORE_ERROR("[freetype]: failed to load freetype library");
        LVN_CORE_ASSERT(false, "failed to load freetype");
        return font;
    }

    if (FT_New_Memory_Face(ft, fontData, fontDataSize, 0, &face))
    {
        LVN_CORE_ERROR("[freetype]: failed to load font face!");
        LVN_CORE_ASSERT(false, "failed to load font face");
        return font;
    }

    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)fontSize);

    int maxDim = (1 + (face->size->metrics.height >> 6)) * ceilf(sqrtf(codepointCount));
    int width = 1;
    while (width < maxDim) width <<= 1;
    int height = width;

    // render glyphs to atlas
    LvnVector<uint8_t> pixels(width * height);
    int penx = 0, peny = 0;
    const int padding = 2;
    const int lineHeight = (face->size->metrics.height >> 6) + padding;

    LvnVector<LvnFontGlyph> glyphs(codepointCount);

    uint32_t loadFlags = FT_LOAD_RENDER;
    if (flags & Lvn_LoadFont_NoHinting)
        loadFlags |= FT_LOAD_NO_HINTING;
    if (flags & Lvn_LoadFont_AutoHinting)
        loadFlags |= FT_LOAD_FORCE_AUTOHINT;
    if (flags & Lvn_LoadFont_TargetLight)
        loadFlags |= FT_LOAD_TARGET_LIGHT;
    if (flags & Lvn_LoadFont_TargetMono)
        loadFlags |= FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME;

    for (uint32_t i = 0; i < codepointCount; i++)
    {
        FT_Load_Char(face, pCodepoints[i], loadFlags);
        FT_Bitmap* bmp = &face->glyph->bitmap;

        if (penx + bmp->width + padding > width)
        {
            penx = padding;
            peny += lineHeight;
        }

        if (bmp->pixel_mode == FT_PIXEL_MODE_MONO && flags & Lvn_LoadFont_TargetMono)
        {
            for (uint32_t row = 0; row < bmp->rows; ++row)
            {
                for (uint32_t col = 0; col < bmp->width; ++col)
                {
                    int byteIndex = col / 8;
                    int bitIndex = 7 - (col % 8);
                    uint8_t byte = bmp->buffer[row * bmp->pitch + byteIndex];
                    bool bitSet = (byte >> bitIndex) & 1;
                    int x = penx + col;
                    int y = peny + row;
                    if (x < width && y < height)
                        pixels[y * width + x] = bitSet ? 255 : 0;
                }
            }
        }
        else
        {
            for (uint32_t row = 0; row < bmp->rows; row++)
            {
                for (uint32_t col = 0; col < bmp->width; col++)
                {
                    int x = penx + col;
                    int y = peny + row;
                    pixels[y * width + x] = bmp->buffer[row * abs(bmp->pitch) + col];
                }
            }
        }


        LvnFontGlyph glyph{};
        glyph.uv.x0 = (float)penx / (float)width;
        glyph.uv.y0 = (float)peny / (float)height;
        glyph.uv.x1 = (float)(penx + bmp->width) / (float)width;
        glyph.uv.y1 = (float)(peny + bmp->rows) / (float)height;

        glyph.size.x = bmp->width;
        glyph.size.y = bmp->rows;
        glyph.bearing.x = face->glyph->bitmap_left;
        glyph.bearing.y = face->glyph->bitmap_top;
        glyph.advance = face->glyph->advance.x >> 6;

        glyphs[i] = glyph;

        penx += bmp->width + padding;
    }

    FT_Done_FreeType(ft);

    LvnImageData atlas{};
    atlas.width = width;
    atlas.height = height;
    atlas.channels = 1;
    atlas.size = width * height;
    atlas.pixels = LvnData<uint8_t>(pixels.data(), pixels.size());

    font.atlas = atlas;
    font.glyphs = LvnData<LvnFontGlyph>(glyphs.data(), glyphs.size());
    font.codepoints = LvnData<uint32_t>(pCodepoints, codepointCount);
    font.fontSize = fontSize;

    return font;
}

LvnFontGlyph fontGetGlyph(const LvnFont& font, uint32_t codepoint)
{
    for (uint32_t i = 0; i < font.glyphs.size(); i++)
    {
        if (font.glyphs[i].unicode == codepoint)
            return font.glyphs[i];
    }

    return font.glyphs[0];
}

uint32_t decodeCodepointUTF8(const char* str, uint32_t* next)
{
    LVN_CORE_ASSERT(next, "next is nullptr");

    const uint8_t *ptr = reinterpret_cast<const uint8_t*>(str);
    uint32_t codepoint = 0x3f;
    *next = 1;

    if ((ptr[0] & 0xf8) == 0xf0)
    {
        // 4-byte sequence
        if (((ptr[1] & 0xc0) ^ 0x80) || ((ptr[2] & 0xc0) ^ 0x80) || ((ptr[3] & 0xc0) ^ 0x80))
            return codepoint;
        codepoint = ((ptr[0] & 0x07) << 18) | ((ptr[1] & 0x3f) << 12) | ((ptr[2] & 0x3f) << 6) | (ptr[3] & 0x3f);
        *next = 4;
    }
    else if ((ptr[0] & 0xf0) == 0xe0)
    {
        // 3-byte sequence
        if (((ptr[1] & 0xc0) ^ 0x80) || ((ptr[2] & 0xc0) ^ 0x80))
            return codepoint;
        codepoint = ((ptr[0] & 0x0f) << 12) | ((ptr[1] & 0x3f) << 6) | (ptr[2] & 0x3f);
        *next = 3;
    }
    else if ((ptr[0] & 0xe0) == 0xc0)
    {
        // 2-byte sequence
        if (((ptr[1] & 0xc0) ^ 0x80))
            return codepoint;
        codepoint = ((ptr[0] & 0x1f) << 6) | (ptr[1] & 0x3f);
        *next = 2;
    }
    else if (ptr[0] < 0x80)
    {
        // 1-byte ASCII
        codepoint = ptr[0];
        *next = 1;
    }

    return codepoint;
}

LvnData<uint32_t> getDefaultSupportedCodepoints()
{
    return lvn::getContext()->defaultCodePoints;
}

void* memAlloc(size_t size)
{
    if (size == 0) { return nullptr; }
    void* allocmem = (*s_MemAllocFunc)(size, s_MemAllocUserData);
    if (!allocmem) { LVN_CORE_ERROR("malloc failure, could not allocate memory!"); LVN_ABORT; }
    memset(allocmem, 0, size);
    if (s_LvnContext) { s_LvnContext->numMemoryAllocations++; }
    return allocmem;
}

void memFree(void* ptr)
{
    if (ptr == nullptr) { return; }
    (*s_MemFreeFunc)(ptr, s_MemAllocUserData);
    if (s_LvnContext) s_LvnContext->numMemoryAllocations--;
}

void* memRealloc(void* ptr, size_t size)
{
    if (!ptr) { return lvn::memAlloc(size); }
    return (*s_MemReallocFunc)(ptr, size, s_MemAllocUserData);
}

void setMemFuncs(LvnMemAllocFunc allocFunc, LvnMemFreeFunc freeFunc, LvnMemReallocFunc reallocFunc, void* userData)
{
    s_MemAllocFunc = allocFunc;
    s_MemFreeFunc = freeFunc;
    s_MemReallocFunc = reallocFunc;
    s_MemAllocUserData = userData;
}

LvnMemAllocFunc getMemAllocFunc()
{
    return s_MemAllocFunc;
}

LvnMemFreeFunc getMemFreeFunc()
{
    return s_MemFreeFunc;
}

LvnMemReallocFunc getMemReallocFunc()
{
    return s_MemReallocFunc;
}

void* getMemUserData()
{
    return s_MemAllocUserData;
}

/* [Logging] */
const static LvnLogPattern s_LogPatterns[] =
{
    { '$', [](LvnLogMessage* msg) -> LvnString { return "\n"; } },
    { 'n', [](LvnLogMessage* msg) -> LvnString { return msg->loggerName; } },
    { 'l', [](LvnLogMessage* msg) -> LvnString { return getLogLevelName(msg->level); }},
    { '#', [](LvnLogMessage* msg) -> LvnString { return getLogLevelColor(msg->level); }},
    { '^', [](LvnLogMessage* msg) -> LvnString { return LVN_LOG_COLOR_RESET; }},
    { 'v', [](LvnLogMessage* msg) -> LvnString { return msg->msg; }},
    { '%', [](LvnLogMessage* msg) -> LvnString { return "%"; } },
    { 'T', [](LvnLogMessage* msg) -> LvnString { return dateGetTimeHHMMSS(); } },
    { 't', [](LvnLogMessage* msg) -> LvnString { return dateGetTime12HHMMSS(); } },
    { 'Y', [](LvnLogMessage* msg) -> LvnString { return dateGetYearStr(); }},
    { 'y', [](LvnLogMessage* msg) -> LvnString { return dateGetYear02dStr(); } },
    { 'm', [](LvnLogMessage* msg) -> LvnString { return dateGetMonthNumStr(); } },
    { 'B', [](LvnLogMessage* msg) -> LvnString { return dateGetMonthName(); } },
    { 'b', [](LvnLogMessage* msg) -> LvnString { return dateGetMonthNameShort(); } },
    { 'd', [](LvnLogMessage* msg) -> LvnString { return dateGetDayNumStr(); } },
    { 'A', [](LvnLogMessage* msg) -> LvnString { return dateGetWeekDayName(); } },
    { 'a', [](LvnLogMessage* msg) -> LvnString { return dateGetWeekDayNameShort(); } },
    { 'H', [](LvnLogMessage* msg) -> LvnString { return dateGetHourNumStr(); } },
    { 'h', [](LvnLogMessage* msg) -> LvnString { return dateGetHour12NumStr(); } },
    { 'M', [](LvnLogMessage* msg) -> LvnString { return dateGetMinuteNumStr(); } },
    { 'S', [](LvnLogMessage* msg) -> LvnString { return dateGetSecondNumStr(); } },
    { 'P', [](LvnLogMessage* msg) -> LvnString { return dateGetTimeMeridiem(); } },
    { 'p', [](LvnLogMessage* msg) -> LvnString { return dateGetTimeMeridiemLower(); }},
};

static LvnResult initLogging(LvnContextCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();
    lvnctx->enableCoreLogging = !createInfo->logging.disableCoreLogging;

    if (createInfo->logging.enableLogging)
    {
        lvnctx->logging = true;

        lvnctx->coreLogger.loggerName = "CORE";

        if (!lvnctx->appName.empty())
            lvnctx->clientLogger.loggerName = lvnctx->appName;
        else
            lvnctx->clientLogger.loggerName = "CLIENT";

        lvnctx->coreLogger.logLevel = lvnctx->clientLogger.logLevel = Lvn_LogLevel_None;
        lvnctx->coreLogger.logPatternFormat = lvnctx->clientLogger.logPatternFormat = LVN_DEFAULT_LOG_PATTERN;
        lvnctx->coreLogger.logPatterns = lvnctx->clientLogger.logPatterns = lvn::logParseFormat(LVN_DEFAULT_LOG_PATTERN);

        #ifdef LVN_PLATFORM_WINDOWS
        enableLogANSIcodeColors();
        #endif

        return Lvn_Result_Success;
    }

    return Lvn_Result_AlreadyCalled;
}

static void terminateLogging()
{
    LvnContext* lvnctx = lvn::getContext();

    if (lvnctx->coreLogger.logfile.logToFile)
    {
        fclose(lvnctx->coreLogger.logfile.fileptr);
        lvnctx->coreLogger.logfile.fileptr = nullptr;
    }
    if (lvnctx->clientLogger.logfile.logToFile)
    {
        fclose(lvnctx->clientLogger.logfile.fileptr);
        lvnctx->clientLogger.logfile.fileptr = nullptr;
    }
}

static LvnVector<LvnLogPattern> logParseFormat(const char* fmt)
{
    if (!fmt || !*fmt) { return {}; }

    LvnVector<LvnLogPattern> patterns;

    for (uint32_t i = 0; i < strlen(fmt) - 1; i++)
    {
        if (fmt[i] != '%') // Other characters in format
        {
            LvnLogPattern pattern = { /* .symbol = */ fmt[i], /* .func = */ nullptr };
            patterns.push_back(pattern);
            continue;
        }

        // find pattern with matching symbol
        for (uint32_t j = 0; j < sizeof(s_LogPatterns) / sizeof(LvnLogPattern); j++)
        {
            if (fmt[i + 1] != s_LogPatterns[j].symbol)
                continue;

            patterns.push_back(s_LogPatterns[j]);
        }

        // find and add user defined patterns
        for (uint32_t j = 0; j < s_LvnContext->userLogPatterns.size(); j++)
        {
            if (fmt[i + 1] != s_LvnContext->userLogPatterns[j].symbol)
                continue;

            patterns.push_back(s_LvnContext->userLogPatterns[j]);
        }

        i++; // incramant past symbol on next character in format
    }

    return patterns;
}

void logEnable(bool enable)
{
    lvn::getContext()->logging = enable;
}

void logEnableCoreLogging(bool enable)
{
    lvn::getContext()->enableCoreLogging = enable;
}

void logSetLevel(LvnLogger* logger, LvnLogLevel level)
{
    logger->logLevel = level;
}

void logSetFileConfig(LvnLogger* logger, bool enable, const char* filename, LvnFileMode filemode)
{
    // if log to file was enabled before, fileptr needs to be closed
    if (logger->logfile.logToFile)
    {
        fclose(logger->logfile.fileptr);
        logger->logfile.fileptr = nullptr;
    }

    logger->logfile.logToFile = enable;
    logger->logfile.filename = filename;
    logger->logfile.filemode = filemode;

    if (enable)
    {
        if (logger->logfile.filename.empty())
        {
            logger->logfile.filename = logger->loggerName + "_logs.txt";
            LVN_CORE_WARN("logSetFileConfig(LvnLogger*, bool enable, const char* filename, LvnFileMode filemode) | filename not set, setting file name to name of the logger: %s_logs.txt", logger->loggerName.c_str());
        }

        const char* filemode = "w";
        if (logger->logfile.filemode == Lvn_FileMode_Write) filemode = "w";
        else if (logger->logfile.filemode == Lvn_FileMode_Append) filemode = "a";

        logger->logfile.fileptr = fopen(logger->logfile.filename.c_str(), filemode);
    }
}

bool logCheckLevel(LvnLogger* logger, LvnLogLevel level)
{
    return (level >= logger->logLevel);
}

void logRenameLogger(LvnLogger* logger, const char* name)
{
    logger->loggerName = name;
}

void logOutputMessage(LvnLogger* logger, LvnLogMessage* msg)
{
    if (!lvn::getContext()->logging) { return; }

    LvnString msgstr; msgstr.reserve(strlen(msg->msg) + 1);

    for (uint32_t i = 0; i < logger->logPatterns.size(); i++)
    {
        if (logger->logPatterns[i].func == nullptr) // no special format character '%' found
        {
            msgstr += logger->logPatterns[i].symbol;
        }
        else // call func of special format
        {
            msgstr += logger->logPatterns[i].func(msg);
        }
    }

    printf("%s", msgstr.c_str());
}

LvnString logFormatMessage(LvnLogger* logger, LvnLogLevel level, const char* msg, bool removeANSI)
{
    LvnLogMessage logMsg{};
    logMsg.msg = msg;
    logMsg.loggerName = logger->loggerName.c_str();
    logMsg.level = level;
    logMsg.timeEpoch = lvn::dateGetSecondsSinceEpoch();

    LvnString msgstr; msgstr.reserve(strlen(msg) + 1);

    for (uint32_t i = 0; i < logger->logPatterns.size(); i++)
    {
        if (removeANSI && (logger->logPatterns[i].symbol == '#' || logger->logPatterns[i].symbol == '^'))
            continue;

        if (logger->logPatterns[i].func == nullptr) // no special format character '%' found
        {
            msgstr += logger->logPatterns[i].symbol;
        }
        else // call func of special format
        {
            msgstr += logger->logPatterns[i].func(&logMsg);
        }
    }

    return msgstr;
}

void logMessage(LvnLogger* logger, LvnLogLevel level, const char* msg)
{
    if (!lvn::getContext()->logging) { return; }

    LvnLogMessage logMsg{};
    logMsg.msg = msg;
    logMsg.loggerName = logger->loggerName.c_str();
    logMsg.level = level;
    logMsg.timeEpoch = lvn::dateGetSecondsSinceEpoch();

    lvn::logOutputMessage(logger, &logMsg);

    if (logger->logfile.logToFile)
    {
        LvnString msgstr; msgstr.reserve(strlen(msg) + 1);

        for (uint32_t i = 0; i < logger->logPatterns.size(); i++)
        {
            if (logger->logPatterns[i].symbol == '#' || logger->logPatterns[i].symbol == '^')
                continue;

            if (logger->logPatterns[i].func == nullptr) // no special format character '%' found
            {
                msgstr += logger->logPatterns[i].symbol;
            }
            else // call func of special format
            {
                msgstr += logger->logPatterns[i].func(&logMsg);
            }
        }

        fprintf(logger->logfile.fileptr, "%s", msgstr.c_str());
    }
}

void logMessageTrace(LvnLogger* logger, const char* fmt, ...)
{
    if (!s_LvnContext || !s_LvnContext->logging) { return; }
    if (!s_LvnContext->enableCoreLogging && logger == &s_LvnContext->coreLogger) { return; }
    if (!lvn::logCheckLevel(logger, Lvn_LogLevel_Trace)) { return; }

    LvnVector<char> buff;

    va_list argptr, argcopy;
    va_start(argptr, fmt);
    va_copy(argcopy, argptr);

    int len = vsnprintf(nullptr, 0, fmt, argptr);
    buff.resize(len + 1);
    vsnprintf(&buff[0], len + 1, fmt, argcopy);
    lvn::logMessage(logger, Lvn_LogLevel_Trace, buff.data());

    va_end(argcopy);
    va_end(argptr);
}

void logMessageDebug(LvnLogger* logger, const char* fmt, ...)
{
    if (!s_LvnContext || !s_LvnContext->logging) { return; }
    if (!s_LvnContext->enableCoreLogging && logger == &s_LvnContext->coreLogger) { return; }
    if (!lvn::logCheckLevel(logger, Lvn_LogLevel_Debug)) { return; }

    LvnVector<char> buff;

    va_list argptr, argcopy;
    va_start(argptr, fmt);
    va_copy(argcopy, argptr);

    int len = vsnprintf(nullptr, 0, fmt, argptr);
    buff.resize(len + 1);
    vsnprintf(&buff[0], len + 1, fmt, argcopy);
    lvn::logMessage(logger, Lvn_LogLevel_Debug, buff.data());

    va_end(argcopy);
    va_end(argptr);
}

void logMessageInfo(LvnLogger* logger, const char* fmt, ...)
{
    if (!s_LvnContext || !s_LvnContext->logging) { return; }
    if (!s_LvnContext->enableCoreLogging && logger == &s_LvnContext->coreLogger) { return; }
    if (!lvn::logCheckLevel(logger, Lvn_LogLevel_Info)) { return; }

    LvnVector<char> buff;

    va_list argptr, argcopy;
    va_start(argptr, fmt);
    va_copy(argcopy, argptr);

    int len = vsnprintf(nullptr, 0, fmt, argptr);
    buff.resize(len + 1);
    vsnprintf(&buff[0], len + 1, fmt, argcopy);
    lvn::logMessage(logger, Lvn_LogLevel_Info, buff.data());

    va_end(argcopy);
    va_end(argptr);
}

void logMessageWarn(LvnLogger* logger, const char* fmt, ...)
{
    if (!s_LvnContext || !s_LvnContext->logging) { return; }
    if (!s_LvnContext->enableCoreLogging && logger == &s_LvnContext->coreLogger) { return; }
    if (!lvn::logCheckLevel(logger, Lvn_LogLevel_Warn)) { return; }

    LvnVector<char> buff;

    va_list argptr, argcopy;
    va_start(argptr, fmt);
    va_copy(argcopy, argptr);

    int len = vsnprintf(nullptr, 0, fmt, argptr);
    buff.resize(len + 1);
    vsnprintf(&buff[0], len + 1, fmt, argcopy);
    lvn::logMessage(logger, Lvn_LogLevel_Warn, buff.data());

    va_end(argcopy);
    va_end(argptr);
}

void logMessageError(LvnLogger* logger, const char* fmt, ...)
{
    if (!s_LvnContext || !s_LvnContext->logging) { return; }
    if (!s_LvnContext->enableCoreLogging && logger == &s_LvnContext->coreLogger) { return; }
    if (!lvn::logCheckLevel(logger, Lvn_LogLevel_Error)) { return; }

    LvnVector<char> buff;

    va_list argptr, argcopy;
    va_start(argptr, fmt);
    va_copy(argcopy, argptr);

    int len = vsnprintf(nullptr, 0, fmt, argptr);
    buff.resize(len + 1);
    vsnprintf(&buff[0], len + 1, fmt, argcopy);
    lvn::logMessage(logger, Lvn_LogLevel_Error, buff.data());

    va_end(argcopy);
    va_end(argptr);
}

void logMessageFatal(LvnLogger* logger, const char* fmt, ...)
{
    if (!s_LvnContext || !s_LvnContext->logging) { return; }
    if (!s_LvnContext->enableCoreLogging && logger == &s_LvnContext->coreLogger) { return; }
    if (!lvn::logCheckLevel(logger, Lvn_LogLevel_Fatal)) { return; }

    LvnVector<char> buff;

    va_list argptr, argcopy;
    va_start(argptr, fmt);
    va_copy(argcopy, argptr);

    int len = vsnprintf(nullptr, 0, fmt, argptr);
    buff.resize(len + 1);
    vsnprintf(&buff[0], len + 1, fmt, argcopy);
    lvn::logMessage(logger, Lvn_LogLevel_Fatal, buff.data());

    va_end(argcopy);
    va_end(argptr);
}

LvnLogger* logGetCoreLogger()
{
    return &lvn::getContext()->coreLogger;
}

LvnLogger* logGetClientLogger()
{
    return &lvn::getContext()->clientLogger;
}

const char* logGetANSIcodeColor(LvnLogLevel level)
{
    switch (level)
    {
        case Lvn_LogLevel_None:     { return LVN_LOG_COLOR_RESET; }
        case Lvn_LogLevel_Trace:    { return LVN_LOG_COLOR_TRACE; }
        case Lvn_LogLevel_Debug:    { return LVN_LOG_COLOR_DEBUG; }
        case Lvn_LogLevel_Info:     { return LVN_LOG_COLOR_INFO; }
        case Lvn_LogLevel_Warn:     { return LVN_LOG_COLOR_WARN; }
        case Lvn_LogLevel_Error:    { return LVN_LOG_COLOR_ERROR; }
        case Lvn_LogLevel_Fatal:    { return LVN_LOG_COLOR_FATAL; }
    }

    return nullptr;
}

LvnResult logSetPatternFormat(LvnLogger* logger, const char* patternfmt)
{
    if (!logger) { return Lvn_Result_Failure; }
    if (!patternfmt || patternfmt[0] == '\0') { return Lvn_Result_Failure; }

    logger->logPatternFormat = patternfmt;

    logger->logPatterns = lvn::logParseFormat(patternfmt);

    return Lvn_Result_Success;
}

LvnResult logAddPatterns(LvnLogPattern* pLogPatterns, uint32_t count)
{
    if (!pLogPatterns) { return Lvn_Result_Failure; }
    if (pLogPatterns->symbol == '\0') { return Lvn_Result_Failure; }

    for (uint32_t i = 0; i < sizeof(s_LogPatterns) / sizeof(LvnLogPattern); i++)
    {
        for (uint32_t j = 0; j < count; j++)
        {
            if (pLogPatterns[j].symbol == s_LogPatterns[i].symbol) { return Lvn_Result_Failure; }
        }
    }

    LvnContext* lvnctx = lvn::getContext();
    lvnctx->userLogPatterns.insert(lvnctx->userLogPatterns.end(), pLogPatterns, pLogPatterns + count);

    return Lvn_Result_Success;
}

LvnResult createLogger(LvnLogger** logger, const LvnLoggerCreateInfo* loggerCreateInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    *logger = lvn::createObject<LvnLogger>(lvnctx, Lvn_Stype_Logger);
    LvnLogger* loggerPtr = *logger;

    loggerPtr->loggerName = loggerCreateInfo->loggerName;
    loggerPtr->logPatternFormat = loggerCreateInfo->format;
    loggerPtr->logLevel = loggerCreateInfo->level;

    loggerPtr->logfile.logToFile = loggerCreateInfo->fileConfig.enableLogToFile;
    loggerPtr->logfile.filename = loggerCreateInfo->fileConfig.filename;
    loggerPtr->logfile.filemode = loggerCreateInfo->fileConfig.filemode;

    if (loggerPtr->logfile.logToFile)
    {
        if (loggerPtr->logfile.filename.empty())
        {
            LVN_CORE_ERROR("createLogger(LvnLogger**, LvnLoggerCreateInfo*) | loggerCreateInfo->fileConfig.filename is empty, cannot log to a file without a valid file path/name");
            return Lvn_Result_Failure;
        }

        const char* filemode = "w";
        if (loggerPtr->logfile.filemode == Lvn_FileMode_Write) filemode = "w";
        else if (loggerPtr->logfile.filemode == Lvn_FileMode_Append) filemode = "a";

        loggerPtr->logfile.fileptr = fopen(loggerPtr->logfile.filename.c_str(), filemode);
    }

    loggerPtr->logPatterns = lvn::logParseFormat(loggerCreateInfo->format.c_str());

    LVN_CORE_TRACE("created logger: (%p), name: \"%s\"", *logger, loggerCreateInfo->loggerName.c_str());
    return Lvn_Result_Success;
}

void destroyLogger(LvnLogger* logger)
{
    if (logger == nullptr) { return; }

    if (logger->logfile.logToFile)
    {
        fclose(logger->logfile.fileptr);
        logger->logfile.fileptr = nullptr;
    }

    LvnContext* lvnctx = lvn::getContext();
    lvn::destroyObject(lvnctx, logger, Lvn_Stype_Logger);
}

// ------------------------------------------------------------
// [SECTION]: Event Functions
// ------------------------------------------------------------

bool dispatchKeyHoldEvent(LvnEvent* event, bool(*func)(LvnKeyHoldEvent*, void*))
{
    if (event->type == Lvn_EventType_KeyHold)
    {
        LvnKeyHoldEvent eventType{};
        eventType.type = Lvn_EventType_KeyHold;
        eventType.category = Lvn_EventCategory_Input | Lvn_EventCategory_Keyboard;
        eventType.name = "LvnKeyHoldEvent";
        eventType.handled = false;
        eventType.keyCode = event->data.code;
        eventType.repeat = event->data.repeat;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchKeyPressedEvent(LvnEvent* event, bool(*func)(LvnKeyPressedEvent*, void*))
{
    if (event->type == Lvn_EventType_KeyPressed)
    {
        LvnKeyPressedEvent eventType{};
        eventType.type = Lvn_EventType_KeyPressed;
        eventType.category = Lvn_EventCategory_Input | Lvn_EventCategory_Keyboard;
        eventType.name = "LvnKeyPressedEvent";
        eventType.handled = false;
        eventType.keyCode = event->data.code;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchKeyReleasedEvent(LvnEvent* event, bool(*func)(LvnKeyReleasedEvent*, void*))
{
    if (event->type == Lvn_EventType_KeyReleased)
    {
        LvnKeyReleasedEvent eventType{};
        eventType.type = Lvn_EventType_KeyReleased;
        eventType.category = Lvn_EventCategory_Input | Lvn_EventCategory_Keyboard;
        eventType.name = "LvnKeyReleasedEvent";
        eventType.handled = false;
        eventType.keyCode = event->data.code;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchKeyTypedEvent(LvnEvent* event, bool(*func)(LvnKeyTypedEvent*, void*))
{
    if (event->type == Lvn_EventType_KeyTyped)
    {
        LvnKeyTypedEvent eventType{};
        eventType.type = Lvn_EventType_KeyTyped;
        eventType.category = Lvn_EventCategory_Input | Lvn_EventCategory_Keyboard;
        eventType.name = "LvnKeyTypedEvent";
        eventType.handled = false;
        eventType.key = event->data.ucode;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchMouseButtonPressedEvent(LvnEvent* event, bool(*func)(LvnMouseButtonPressedEvent*, void*))
{
    if (event->type == Lvn_EventType_MouseButtonPressed)
    {
        LvnMouseButtonPressedEvent eventType{};
        eventType.type = Lvn_EventType_MouseButtonPressed;
        eventType.category = Lvn_EventCategory_Input | Lvn_EventCategory_MouseButton | Lvn_EventCategory_Mouse;
        eventType.name = "LvnMouseButtonPressedEvent";
        eventType.handled = false;
        eventType.buttonCode = event->data.code;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchMouseButtonReleasedEvent(LvnEvent* event, bool(*func)(LvnMouseButtonReleasedEvent*, void*))
{
    if (event->type == Lvn_EventType_MouseButtonReleased)
    {
        LvnMouseButtonReleasedEvent eventType{};
        eventType.type = Lvn_EventType_MouseButtonReleased;
        eventType.category = Lvn_EventCategory_Input | Lvn_EventCategory_MouseButton | Lvn_EventCategory_Mouse;
        eventType.name = "LvnMouseButtonReleasedEvent";
        eventType.handled = false;
        eventType.buttonCode = event->data.code;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchMouseMovedEvent(LvnEvent* event, bool(*func)(LvnMouseMovedEvent*, void*))
{
    if (event->type == Lvn_EventType_MouseMoved)
    {
        LvnMouseMovedEvent eventType{};
        eventType.type = Lvn_EventType_MouseMoved;
        eventType.category = Lvn_EventCategory_Input | Lvn_EventCategory_Mouse;
        eventType.name = "LvnMouseMovedEvent";
        eventType.handled = false;
        eventType.x = event->data.xd;
        eventType.y = event->data.yd;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchMouseScrolledEvent(LvnEvent* event, bool(*func)(LvnMouseScrolledEvent*, void*))
{
    if (event->type == Lvn_EventType_MouseScrolled)
    {
        LvnMouseScrolledEvent eventType{};
        eventType.type = Lvn_EventType_MouseScrolled;
        eventType.category = Lvn_EventCategory_Input | Lvn_EventCategory_MouseButton | Lvn_EventCategory_Mouse;
        eventType.name = "LvnMouseScrolledEvent";
        eventType.handled = false;
        eventType.x = static_cast<float>(event->data.xd);
        eventType.y = static_cast<float>(event->data.yd);

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchWindowCloseEvent(LvnEvent* event, bool(*func)(LvnWindowCloseEvent*, void*))
{
    if (event->type == Lvn_EventType_WindowClose)
    {
        LvnWindowCloseEvent eventType{};
        eventType.type = Lvn_EventType_WindowClose;
        eventType.category = Lvn_EventCategory_Window;
        eventType.name = "LvnWindowCloseEvent";
        eventType.handled = false;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchWindowFocusEvent(LvnEvent* event, bool(*func)(LvnWindowFocusEvent*, void*))
{
    if (event->type == Lvn_EventType_WindowFocus)
    {
        LvnWindowFocusEvent eventType{};
        eventType.type = Lvn_EventType_WindowFocus;
        eventType.category = Lvn_EventCategory_Window;
        eventType.name = "LvnWindowFocusEvent";
        eventType.handled = false;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchWindowFramebufferResizeEvent(LvnEvent* event, bool(*func)(LvnWindowFramebufferResizeEvent*, void*))
{
    if (event->type == Lvn_EventType_WindowFramebufferResize)
    {
        LvnWindowFramebufferResizeEvent eventType{};
        eventType.type = Lvn_EventType_WindowFramebufferResize;
        eventType.category = Lvn_EventCategory_Window;
        eventType.name = "LvnWindowFramebufferResizeEvent";
        eventType.handled = false;
        eventType.width = event->data.x;
        eventType.height = event->data.y;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchWindowLostFocusEvent(LvnEvent* event, bool(*func)(LvnWindowLostFocusEvent*, void*))
{
    if (event->type == Lvn_EventType_WindowLostFocus)
    {
        LvnWindowLostFocusEvent eventType{};
        eventType.type = Lvn_EventType_WindowLostFocus;
        eventType.category = Lvn_EventCategory_Window;
        eventType.name = "LvnWindowLostFocusEvent";
        eventType.handled = false;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchWindowMovedEvent(LvnEvent* event, bool(*func)(LvnWindowMovedEvent*, void*))
{
    if (event->type == Lvn_EventType_WindowMoved)
    {
        LvnWindowMovedEvent eventType{};
        eventType.type = Lvn_EventType_WindowMoved;
        eventType.category = Lvn_EventCategory_Window;
        eventType.name = "LvnWindowMovedEvent";
        eventType.handled = false;
        eventType.x = event->data.x;
        eventType.y = event->data.y;

        return func(&eventType, event->userData);
    }

    return false;
}
bool dispatchWindowResizeEvent(LvnEvent* event, bool(*func)(LvnWindowResizeEvent*, void*))
{

    if (event->type == Lvn_EventType_WindowResize)
    {
        LvnWindowResizeEvent eventType{};
        eventType.type = Lvn_EventType_WindowResize;
        eventType.category = Lvn_EventCategory_Window;
        eventType.name = "LvnWindowResizeEvent";
        eventType.handled = false;
        eventType.width = event->data.x;
        eventType.height = event->data.y;

        return func(&eventType, event->userData);
    }

    return false;
}


// ------------------------------------------------------------
// [SECTION]: Window Functions
// ------------------------------------------------------------

LvnWindowApi getWindowApi()
{
    return lvn::getContext()->windowapi;
}

const char* getWindowApiName()
{
    switch (lvn::getContext()->windowapi)
    {
        case Lvn_WindowApi_None:  { return "None";  }
        case Lvn_WindowApi_glfw:  { return "glfw";  }
        // case Lvn_WindowApi_win32: { return "win32"; }
    }

    LVN_CORE_ERROR("Unknown Windows API selected!");
    return LVN_EMPTY_STR;
}

LvnResult createWindow(LvnWindow** window, const LvnWindowCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    if (createInfo->width < 0 || createInfo->height < 0)
    {
        LVN_CORE_ERROR("createWindow(LvnWindow**, LvnWindowCreateInfo*) | cannot create window with negative dimensions (w:%d,h:%d)", createInfo->width, createInfo->height);
        return Lvn_Result_Failure;
    }

    *window = lvn::createObject<LvnWindow>(lvnctx, Lvn_Stype_Window);

    LVN_CORE_TRACE("created window: (%p), \"%s\" (w:%d,h:%d)", *window, createInfo->title.c_str(), createInfo->width, createInfo->height);
    return lvnctx->windowContext.createWindow(*window, createInfo);
}

void destroyWindow(LvnWindow* window)
{
    if (window == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();
    lvnctx->windowContext.destroyWindow(window);
    lvn::destroyObject(lvnctx, window, Lvn_Stype_Window);
}

LvnWindowCreateInfo configWindowInit(const char* title, int width, int height)
{
    LvnWindowCreateInfo windowCreateInfo{};
    windowCreateInfo.width = width;
    windowCreateInfo.height = height;
    windowCreateInfo.title = title;
    windowCreateInfo.minWidth = 0;
    windowCreateInfo.minHeight = 0;
    windowCreateInfo.maxWidth = -1;
    windowCreateInfo.maxHeight = -1;
    windowCreateInfo.fullscreen = false;
    windowCreateInfo.resizable = true;
    windowCreateInfo.vSync = false;
    windowCreateInfo.pIcons = nullptr;
    windowCreateInfo.iconCount = 0;
    windowCreateInfo.eventCallBack = nullptr;
    windowCreateInfo.userData = nullptr;

    return windowCreateInfo;
}

void windowUpdate(LvnWindow* window)
{
    lvn::getContext()->windowContext.updateWindow(window);
}

bool windowOpen(LvnWindow* window)
{
    return lvn::getContext()->windowContext.windowOpen(window);
}

void windowPollEvents()
{
    lvn::getContext()->windowContext.windowPollEvents();
}

LvnPair<int> windowGetDimensions(LvnWindow* window)
{
    return lvn::getContext()->windowContext.getWindowSize(window);
}

int windowGetWidth(LvnWindow* window)
{
    return lvn::getContext()->windowContext.getWindowWidth(window);
}

int windowGetHeight(LvnWindow* window)
{
    return lvn::getContext()->windowContext.getWindowHeight(window);
}

void windowSetEventCallback(LvnWindow* window, void (*callback)(LvnEvent*), void* userData)
{
    window->data.eventCallBackFn = callback;
    window->data.userData = userData;
}

void windowSetVSync(LvnWindow* window, bool enable)
{
    lvn::getContext()->windowContext.setWindowVSync(window, enable);
}

bool windowGetVSync(LvnWindow* window)
{
    return lvn::getContext()->windowContext.getWindowVSync(window);
}

void* windowGetNativeWindow(LvnWindow* window)
{
    return window->nativeWindow;
}

LvnRenderPass* windowGetRenderPass(LvnWindow* window)
{
    return &window->renderPass;
}

void windowSetContextCurrent(LvnWindow* window)
{
    lvn::getContext()->windowContext.setWindowContextCurrent(window);
}

// ------------------------------------------------------------
// [SECTION]: Input Functions
// ------------------------------------------------------------

bool keyPressed(LvnWindow* window, int keycode)
{
    return lvn::getContext()->windowContext.keyPressed(window, keycode);
}

bool keyReleased(LvnWindow* window, int keycode)
{
    return lvn::getContext()->windowContext.keyReleased(window, keycode);
}

bool mouseButtonPressed(LvnWindow* window, int button)
{
    return lvn::getContext()->windowContext.mouseButtonPressed(window, button);
}

bool mouseButtonReleased(LvnWindow* window, int button)
{
    return lvn::getContext()->windowContext.mouseButtonReleased(window, button);
}

LvnPair<float> mouseGetPos(LvnWindow* window)
{
    return lvn::getContext()->windowContext.getMousePos(window);
}

void mouseGetPos(LvnWindow* window, float* xpos, float* ypos)
{
    lvn::getContext()->windowContext.getMousePosPtr(window, xpos, ypos);
}

float mouseGetX(LvnWindow* window)
{
    return lvn::getContext()->windowContext.getMouseX(window);
}

float mouseGetY(LvnWindow* window)
{
    return lvn::getContext()->windowContext.getMouseY(window);
}

void mouseSetCursor(LvnWindow* window, LvnMouseCursor cursor)
{
    lvn::getContext()->windowContext.setMouseCursor(window, cursor);
}

void mouseSetInputMode(LvnWindow* window, LvnMouseInputMode mode)
{
    lvn::getContext()->windowContext.SetMouseInputMode(window, mode);
}

LvnPair<int> windowGetPos(LvnWindow* window)
{
    return lvn::getContext()->windowContext.getWindowPos(window);
}

void windowGetPos(LvnWindow* window, int* xpos, int* ypos)
{
    lvn::getContext()->windowContext.getWindowPosPtr(window, xpos, ypos);
}

LvnPair<int> windowGetSize(LvnWindow* window)
{
    return lvn::getContext()->windowContext.getWindowSize(window);
}

void windowGetSize(LvnWindow* window, int* width, int* height)
{
    lvn::getContext()->windowContext.getWindowSizePtr(window, width, height);
}

// ------------------------------------------------------------
// [SECTION]: Graphics Functions
// ------------------------------------------------------------

LvnGraphicsApi getGraphicsApi()
{
    return lvn::getContext()->graphicsapi;
}

const char* getGraphicsApiName()
{
    switch (lvn::getContext()->graphicsapi)
    {
        case Lvn_GraphicsApi_None:   { return "None";   }
        case Lvn_GraphicsApi_vulkan: { return "vulkan"; }
        case Lvn_GraphicsApi_opengl: { return "opengl"; }
    }

    LVN_CORE_ERROR("Unknown Graphics API selected!");
    return LVN_EMPTY_STR;
}

void getPhysicalDevices(LvnPhysicalDevice** pPhysicalDevices, uint32_t* deviceCount)
{
    uint32_t getDeviceCount;
    lvn::getContext()->graphicsContext.getPhysicalDevices(nullptr, &getDeviceCount);

    if (deviceCount != nullptr)
        *deviceCount = getDeviceCount;

    if (pPhysicalDevices == nullptr)
        return;

    lvn::getContext()->graphicsContext.getPhysicalDevices(pPhysicalDevices, &getDeviceCount);

    return;
}

LvnPhysicalDeviceProperties getPhysicalDeviceProperties(LvnPhysicalDevice* physicalDevice)
{
    return physicalDevice->properties;
}

LvnPhysicalDeviceFeatures getPhysicalDeviceFeatures(LvnPhysicalDevice* physicalDevice)
{
    return physicalDevice->features;
}

LvnResult checkPhysicalDeviceSupport(LvnPhysicalDevice* physicalDevice)
{
    if (physicalDevice == nullptr)
    {
        LVN_CORE_ERROR("cannot check physical device support, physicalDevice is nullptr");
        return Lvn_Result_Failure;
    }

    return lvn::getContext()->graphicsContext.checkPhysicalDeviceSupport(physicalDevice);
}

LvnResult setPhysicalDevice(LvnPhysicalDevice* physicalDevice)
{
    if (physicalDevice == nullptr)
    {
        LVN_CORE_ERROR("cannot set physical device, physicalDevice is nullptr");
        return Lvn_Result_Failure;
    }

    return lvn::getContext()->graphicsContext.setPhysicalDevice(physicalDevice);
}

LvnClipRegion getRenderClipRegionEnum()
{
    return lvn::getContext()->matrixClipRegion;
}

void renderCmdDraw(LvnWindow* window, uint32_t vertexCount)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdDraw(window, vertexCount);
}

void renderCmdDrawIndexed(LvnWindow* window, uint32_t indexCount)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdDrawIndexed(window, indexCount);
}

void renderCmdDrawInstanced(LvnWindow* window, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstInstance)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdDrawInstanced(window, vertexCount, instanceCount, firstInstance);
}

void renderCmdDrawIndexedInstanced(LvnWindow* window, uint32_t indexCount, uint32_t instanceCount, uint32_t firstInstance)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdDrawIndexedInstanced(window, indexCount, instanceCount, firstInstance);
}

void renderCmdSetStencilReference(uint32_t reference)
{

}

void renderCmdSetStencilMask(uint32_t compareMask, uint32_t writeMask)
{

}

void renderBeginNextFrame(LvnWindow* window)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderBeginNextFrame(window);
}

void renderDrawSubmit(LvnWindow* window)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderDrawSubmit(window);
}

void renderBeginCommandRecording(LvnWindow* window)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderBeginCommandRecording(window);
}

void renderEndCommandRecording(LvnWindow* window)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderEndCommandRecording(window);
}

void renderCmdBeginRenderPass(LvnWindow* window, float r, float g, float b, float a)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdBeginRenderPass(window, r, g, b, a);
}

void renderCmdEndRenderPass(LvnWindow* window)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdEndRenderPass(window);
}

void renderCmdBindPipeline(LvnWindow* window, LvnPipeline* pipeline)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdBindPipeline(window, pipeline);
}

void renderCmdBindVertexBuffer(LvnWindow* window, uint32_t firstBinding, uint32_t bindingCount, LvnBuffer** pBuffers, uint64_t* pOffsets)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    uint64_t offsets[] = {0};
    lvn::getContext()->graphicsContext.renderCmdBindVertexBuffer(window, firstBinding, bindingCount, pBuffers, pOffsets ? pOffsets : offsets);
}

void renderCmdBindIndexBuffer(LvnWindow* window, LvnBuffer* buffer, uint64_t offset)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdBindIndexBuffer(window, buffer, offset);
}

void renderCmdBindDescriptorSets(LvnWindow* window, LvnPipeline* pipeline, uint32_t firstSetIndex, uint32_t descriptorSetCount, LvnDescriptorSet** pDescriptorSets)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdBindDescriptorSets(window, pipeline, firstSetIndex, descriptorSetCount, pDescriptorSets);
}

void renderCmdBeginFrameBuffer(LvnWindow* window, LvnFrameBuffer* frameBuffer)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdBeginFrameBuffer(window, frameBuffer);
}

void renderCmdEndFrameBuffer(LvnWindow* window, LvnFrameBuffer* frameBuffer)
{
    int width, height;
    lvn::windowGetSize(window, &width, &height);
    if (width * height <= 0) { return; }

    lvn::getContext()->graphicsContext.renderCmdEndFrameBuffer(window, frameBuffer);
}

LvnResult createShaderFromSrc(LvnShader** shader, const LvnShaderCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    if (createInfo->vertexSrc.empty())
    {
        LVN_CORE_ERROR("createShaderFromSrc(LvnShader**, LvnShaderCreateInfo*) | createInfo->vertexSrc is nullptr, cannot create shader without the vertex shader source");
        return Lvn_Result_Failure;
    }

    if (createInfo->fragmentSrc.empty())
    {
        LVN_CORE_ERROR("createShaderFromSrc(LvnShader**, LvnShaderCreateInfo*) | createInfo->fragmentSrc is nullptr, cannot create shader without the fragment shader source");
        return Lvn_Result_Failure;
    }

    *shader = lvn::createObject<LvnShader>(lvnctx, Lvn_Stype_Shader);

    LVN_CORE_TRACE("created shader (from source): (%p)", *shader);
    return lvnctx->graphicsContext.createShaderFromSrc(*shader, createInfo);
}

LvnResult createShaderFromFileSrc(LvnShader** shader, const LvnShaderCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    if (createInfo->vertexSrc.empty())
    {
        LVN_CORE_ERROR("createShaderFromFileSrc(LvnShader**, LvnShaderCreateInfo*) | createInfo->vertexSrc is nullptr, cannot create shader without the vertex shader source");
        return Lvn_Result_Failure;
    }

    if (createInfo->fragmentSrc.empty())
    {
        LVN_CORE_ERROR("createShaderFromFileSrc(LvnShader**, LvnShaderCreateInfo*) | createInfo->fragmentSrc is nullptr, cannot create shader without the fragment shader source");
        return Lvn_Result_Failure;
    }

    *shader = lvn::createObject<LvnShader>(lvnctx, Lvn_Stype_Shader);

    LVN_CORE_TRACE("created shader (from source file): (%p), vertex file: %s, fragment file: %s", *shader, createInfo->vertexSrc.c_str(), createInfo->fragmentSrc.c_str());
    return lvnctx->graphicsContext.createShaderFromFileSrc(*shader, createInfo);
}

LvnResult createShaderFromFileBin(LvnShader** shader, const LvnShaderCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    if (createInfo->vertexSrc.empty())
    {
        LVN_CORE_ERROR("createShaderFileBin(LvnShader**, LvnShaderCreateInfo*) | createInfo->vertexSrc is nullptr, cannot create shader without the vertex shader source");
        return Lvn_Result_Failure;
    }

    if (createInfo->fragmentSrc.empty())
    {
        LVN_CORE_ERROR("createShaderFileBin(LvnShader**, LvnShaderCreateInfo*) | createInfo->fragmentSrc is nullptr, cannot create shader without the fragment shader source");
        return Lvn_Result_Failure;
    }

    *shader = lvn::createObject<LvnShader>(lvnctx, Lvn_Stype_Shader);

    LVN_CORE_TRACE("created shader (from binary file): (%p), vertex file: %s, fragment file: %s", *shader, createInfo->vertexSrc.c_str(), createInfo->fragmentSrc.c_str());
    return lvnctx->graphicsContext.createShaderFromFileBin(*shader, createInfo);
}

LvnResult createDescriptorLayout(LvnDescriptorLayout** descriptorLayout, const LvnDescriptorLayoutCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    if (!createInfo->descriptorBindingCount)
    {
        LVN_CORE_ERROR("createDescriptorLayout(LvnDescriptorLayout**, LvnDescriptorLayoutCreateInfo*) | createInfo->descriptorBindingCount is 0, cannot create descriptor layout without the descriptor bindings count");
        return Lvn_Result_Failure;
    }

    if (!createInfo->pDescriptorBindings)
    {
        LVN_CORE_ERROR("createDescriptorLayout(LvnDescriptorLayout**, LvnDescriptorLayoutCreateInfo*) | createInfo->pDescriptorBindings is nullptr, cannot create descriptor layout without the pointer to the array of descriptor bindings");
        return Lvn_Result_Failure;
    }

    for (uint32_t i = 0; i < createInfo->descriptorBindingCount; i++)
    {
        if (createInfo->pDescriptorBindings[i].maxAllocations == 0)
            LVN_CORE_WARN("createDescriptorLayout(LvnDescriptorLayout**, LvnDescriptorLayoutCreateInfo*) | createInfo->pDescriptorBindings[%u].maxAllocations is 0, no descriptors will be allocated for this binding which may not be intentional", i);

        if (createInfo->pDescriptorBindings[i].descriptorCount == 0)
            LVN_CORE_WARN("createDescriptorLayout(LvnDescriptorLayout**, LvnDescriptorLayoutCreateInfo*) | createInfo->pDescriptorBindings[%u].descriptorCount is 0, no descriptors will be created for this binding which may not be intentional", i);
    }

    *descriptorLayout = lvn::createObject<LvnDescriptorLayout>(lvnctx, Lvn_Stype_DescriptorLayout);

    LvnDescriptorLayout* descriptorLayoutPtr = *descriptorLayout;
    descriptorLayoutPtr->descriptorSets.resize(createInfo->maxSets);
    descriptorLayoutPtr->descriptorSetIndex = 0;

    LVN_CORE_TRACE("created descriptorLayout: (%p), descriptor binding count: %u", *descriptorLayout, createInfo->descriptorBindingCount);
    return lvnctx->graphicsContext.createDescriptorLayout(*descriptorLayout, createInfo);
}

LvnResult allocateDescriptorSet(LvnDescriptorSet** descriptorSet, LvnDescriptorLayout* descriptorLayout)
{
    LvnContext* lvnctx = lvn::getContext();

    *descriptorSet = &descriptorLayout->descriptorSets[descriptorLayout->descriptorSetIndex++];

    LVN_CORE_TRACE("allocated descriptorSet: (%p) from descriptorLayout: (%p)", *descriptorSet, descriptorLayout);
    return lvnctx->graphicsContext.allocateDescriptorSet(*descriptorSet, descriptorLayout);
}

LvnResult createPipeline(LvnPipeline** pipeline, const LvnPipelineCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    // vertex binding descriptions
    if (!createInfo->pVertexBindingDescriptions)
    {
        LVN_CORE_ERROR("createBuffer(LvnBuffer*, LvnBufferCreateInfo*) | createInfo->pVertexBindingDescriptions is nullptr; cannot create vertex buffer without the vertex binding descriptions");
        return Lvn_Result_Failure;
    }
    else if (!createInfo->vertexBindingDescriptionCount)
    {
        LVN_CORE_ERROR("createBuffer(LvnBuffer*, LvnBufferCreateInfo*) | createInfo->vertexBindingDescriptionCount is 0; cannot create vertex buffer without the vertex binding descriptions");
        return Lvn_Result_Failure;
    }

    // vertex attributes
    if (!createInfo->pVertexAttributes)
    {
        LVN_CORE_ERROR("createBuffer(LvnBuffer*, LvnBufferCreateInfo*) | createInfo->pVertexAttributes is nullptr; cannot create vertex buffer without the vertex attributes");
        return Lvn_Result_Failure;
    }
    else if (!createInfo->vertexAttributeCount)
    {
        LVN_CORE_ERROR("createBuffer(LvnBuffer*, LvnBufferCreateInfo*) | createInfo->vertexAttributeCount is 0; cannot create vertex buffer without the vertex attributes");
        return Lvn_Result_Failure;
    }

    for (uint32_t i = 0; i < createInfo->vertexAttributeCount; i++)
    {
        if (createInfo->pVertexAttributes[i].format == Lvn_AttributeFormat_Undefined)
        {
            LVN_CORE_ERROR("createBuffer(LvnBuffer*, LvnBufferCreateInfo*) | createInfo->pVertexAttributes[%d].type is Lvn_AttributeFormat_Undefined, cannot create vertex buffer without a vertex data type", i);
            return Lvn_Result_Failure;
        }
    }

    *pipeline = lvn::createObject<LvnPipeline>(lvnctx, Lvn_Stype_Pipeline);

    LVN_CORE_TRACE("created pipeline: (%p)", *pipeline);
    return lvnctx->graphicsContext.createPipeline(*pipeline, createInfo);
}

LvnResult createFrameBuffer(LvnFrameBuffer** frameBuffer, const LvnFrameBufferCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    if (createInfo->pColorAttachments == nullptr)
    {
        LVN_CORE_ERROR("createFrameBuffer(LvnFrameBuffer**, LvnFrameBufferCreateInfo*) | createInfo->pColorAttachments is nullptr, cannot create framebuffer without one or more color attachments");
        return Lvn_Result_Failure;
    }

    uint32_t totalAttachments = createInfo->colorAttachmentCount + (createInfo->depthAttachment != nullptr ? 1 : 0);

    for (uint32_t i = 0; i < createInfo->colorAttachmentCount; i++)
    {
        if (createInfo->pColorAttachments[i].index >= totalAttachments)
        {
            LVN_CORE_ERROR("createFrameBuffer(LvnFrameBuffer**, LvnFrameBufferCreateInfo*) | createInfo->pColorAttachments[%u].index is greater than or equal to total attachments, color attachment index must be less than the total number of attachments", i);
            return Lvn_Result_Failure;
        }
        if (createInfo->depthAttachment != nullptr && createInfo->pColorAttachments[i].index == createInfo->depthAttachment->index)
        {
            LVN_CORE_ERROR("createFrameBuffer(LvnFrameBuffer**, LvnFrameBufferCreateInfo*) | createInfo->pColorAttachments[%u].index has the same value as createInfo->depthAttachment->index, color attachment index must not be the same as the depth attachment index", i);
            return Lvn_Result_Failure;
        }
    }

    if (createInfo->depthAttachment != nullptr)
    {
        if (createInfo->depthAttachment->index >= totalAttachments)
        {
            LVN_CORE_ERROR("createFrameBuffer(LvnFrameBuffer**, LvnFrameBufferCreateInfo*) | createInfo->pColorAttachments[%u].index is greater than or equal to total attachments, depth attachment index must be less than the total number of attachments");
            return Lvn_Result_Failure;
        }
    }

    *frameBuffer = lvn::createObject<LvnFrameBuffer>(lvnctx, Lvn_Stype_FrameBuffer);

    LVN_CORE_TRACE("created framebuffer: (%p)", *frameBuffer);
    return lvnctx->graphicsContext.createFrameBuffer(*frameBuffer, createInfo);
}

LvnResult createBuffer(LvnBuffer** buffer, const LvnBufferCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    // check valid buffer type
    if (createInfo->type == Lvn_BufferType_Unknown)
    {
        LVN_CORE_ERROR("createBuffer(LvnBuffer*, LvnBufferCreateInfo*) | createInfo->type is \'Lvn_BufferType_Unknown\'; cannot create vertex buffer without knowing the type of buffer usage");
        return Lvn_Result_Failure;
    }

    *buffer = lvn::createObject<LvnBuffer>(lvnctx, Lvn_Stype_Buffer);

    LVN_CORE_TRACE("created buffer: (%p)", *buffer);
    return lvnctx->graphicsContext.createBuffer(*buffer, createInfo);
}

LvnResult createSampler(LvnSampler** sampler, const LvnSamplerCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    *sampler = lvn::createObject<LvnSampler>(lvnctx, Lvn_Stype_Sampler);

    LVN_CORE_TRACE("created sampler: (%p)");
    return lvnctx->graphicsContext.createSampler(*sampler, createInfo);
}

LvnResult createTexture(LvnTexture** texture, const LvnTextureCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    *texture = lvn::createObject<LvnTexture>(lvnctx, Lvn_Stype_Texture);

    LVN_CORE_TRACE("created texture: (%p) using image data: (%p), (w:%u,h:%u,ch:%u), total size: %u bytes",
        *texture,
        createInfo->imageData.pixels.data(),
        createInfo->imageData.width,
        createInfo->imageData.height,
        createInfo->imageData.channels,
        createInfo->imageData.pixels.memsize());

    return lvnctx->graphicsContext.createTexture(*texture, createInfo);
}

LvnResult createTexture(LvnTexture** texture, const LvnTextureSamplerCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    *texture = lvn::createObject<LvnTexture>(lvnctx, Lvn_Stype_Texture);

    LVN_CORE_TRACE("created texture (seperate sampler): (%p) using image data: (%p), (w:%u,h:%u,ch:%u), total size: %u bytes, sampler object used: (%p)",
        *texture,
        createInfo->imageData.pixels.data(),
        createInfo->imageData.width,
        createInfo->imageData.height,
        createInfo->imageData.channels,
        createInfo->imageData.pixels.memsize(),
        createInfo->sampler);

    return lvnctx->graphicsContext.createTextureSampler(*texture, createInfo);
}

LvnResult createCubemap(LvnCubemap** cubemap, const LvnCubemapCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    if (createInfo->posx.pixels.data() == nullptr)
    {
        LVN_CORE_ERROR("createCubemap(LvnCubemap**, LvnCubemapCreateInfo*) | createInfo->posx.pixels does not point to a valid pointer array");
        return Lvn_Result_Failure;
    }
    if (createInfo->negx.pixels.data() == nullptr)
    {
        LVN_CORE_ERROR("createCubemap(LvnCubemap**, LvnCubemapCreateInfo*) | createInfo->negx.pixels does not point to a valid pointer array");
        return Lvn_Result_Failure;
    }
    if (createInfo->posy.pixels.data() == nullptr)
    {
        LVN_CORE_ERROR("createCubemap(LvnCubemap**, LvnCubemapCreateInfo*) | createInfo->posy.pixels does not point to a valid pointer array");
        return Lvn_Result_Failure;
    }
    if (createInfo->negy.pixels.data() == nullptr)
    {
        LVN_CORE_ERROR("createCubemap(LvnCubemap**, LvnCubemapCreateInfo*) | createInfo->negy.pixels does not point to a valid pointer array");
        return Lvn_Result_Failure;
    }
    if (createInfo->posz.pixels.data() == nullptr)
    {
        LVN_CORE_ERROR("createCubemap(LvnCubemap**, LvnCubemapCreateInfo*) | createInfo->posz.pixels does not point to a valid pointer array");
        return Lvn_Result_Failure;
    }
    if (createInfo->negz.pixels.data() == nullptr)
    {
        LVN_CORE_ERROR("createCubemap(LvnCubemap**, LvnCubemapCreateInfo*) | createInfo->negz.pixels does not point to a valid pointer array");
        return Lvn_Result_Failure;
    }

    // if(!(createInfo->posx.width * createInfo->posx.height ==
    //  createInfo->negx.width * createInfo->negx.height ==
    //  createInfo->posy.width * createInfo->posy.height ==
    //  createInfo->negy.width * createInfo->negy.height ==
    //  createInfo->posz.width * createInfo->posz.height ==
    //  createInfo->negz.width * createInfo->negz.height))
    // {
    //  LVN_CORE_ERROR("createCubemap(LvnCubemap**, LvnCubemapCreateInfo*) | not all images have the same dimensions, all cubemap images must have the same width and height");
    //  return Lvn_Result_Failure;
    // }

    *cubemap = lvn::createObject<LvnCubemap>(lvnctx, Lvn_Stype_Cubemap);

    LVN_CORE_TRACE("created cubemap: (%p)", *cubemap);
    return lvnctx->graphicsContext.createCubemap(*cubemap, createInfo);
}

LvnResult createCubemap(LvnCubemap** cubemap, const LvnCubemapHdrCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    if (createInfo->hdr.pixels.data() == nullptr)
    {
        LVN_CORE_ERROR("createCubemap(LvnCubemap**, LvnCubemapHdrCreateInfo*) | createInfo->hdr.pixels does not point to a valid pointer array");
        return Lvn_Result_Failure;
    }

    *cubemap = lvn::createObject<LvnCubemap>(lvnctx, Lvn_Stype_Cubemap);

    LVN_CORE_TRACE("created cubemap (%p) from hdr image (%p)", *cubemap, createInfo->hdr.pixels.data());
    return lvnctx->graphicsContext.createCubemapHdr(*cubemap, createInfo);
}

void destroyShader(LvnShader* shader)
{
    if (shader == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();

    lvnctx->graphicsContext.destroyShader(shader);
    lvn::destroyObject(lvnctx, shader, Lvn_Stype_Shader);
}

void destroyDescriptorLayout(LvnDescriptorLayout* descriptorLayout)
{
    if (descriptorLayout == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();

    lvnctx->graphicsContext.destroyDescriptorLayout(descriptorLayout);
    lvn::destroyObject(lvnctx, descriptorLayout, Lvn_Stype_DescriptorLayout);
}

void destroyPipeline(LvnPipeline* pipeline)
{
    if (pipeline == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();

    lvnctx->graphicsContext.destroyPipeline(pipeline);
    lvn::destroyObject(lvnctx, pipeline, Lvn_Stype_Pipeline);
}

void destroyFrameBuffer(LvnFrameBuffer* frameBuffer)
{
    if (frameBuffer == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();

    lvnctx->graphicsContext.destroyFrameBuffer(frameBuffer);
    lvn::destroyObject(lvnctx, frameBuffer, Lvn_Stype_FrameBuffer);
}

void destroyBuffer(LvnBuffer* buffer)
{
    if (buffer == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();

    lvnctx->graphicsContext.destroyBuffer(buffer);
    lvn::destroyObject(lvnctx, buffer, Lvn_Stype_Buffer);
}

void destroySampler(LvnSampler* sampler)
{
    if (sampler == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();

    lvnctx->graphicsContext.destroySampler(sampler);
    lvn::destroyObject(lvnctx, sampler, Lvn_Stype_Sampler);
}

void destroyTexture(LvnTexture* texture)
{
    if (texture == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();

    lvnctx->graphicsContext.destroyTexture(texture);
    lvn::destroyObject(lvnctx, texture, Lvn_Stype_Texture);
}

void destroyCubemap(LvnCubemap* cubemap)
{
    if (cubemap == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();

    lvnctx->graphicsContext.destroyCubemap(cubemap);
    lvn::destroyObject(lvnctx, cubemap, Lvn_Stype_Cubemap);
}

uint32_t getAttributeFormatSize(LvnAttributeFormat format)
{
    switch (format)
    {
        case Lvn_AttributeFormat_Undefined:        { return 0; }
        case Lvn_AttributeFormat_Scalar_f32:       { return sizeof(float); }
        case Lvn_AttributeFormat_Scalar_f64:       { return sizeof(double); }
        case Lvn_AttributeFormat_Scalar_i32:       { return sizeof(int32_t); }
        case Lvn_AttributeFormat_Scalar_ui32:      { return sizeof(uint32_t); }
        case Lvn_AttributeFormat_Scalar_i8:        { return sizeof(int8_t); }
        case Lvn_AttributeFormat_Scalar_ui8:       { return sizeof(uint8_t); }
        case Lvn_AttributeFormat_Vec2_f32:         { return 2 * sizeof(float); }
        case Lvn_AttributeFormat_Vec3_f32:         { return 3 * sizeof(float); }
        case Lvn_AttributeFormat_Vec4_f32:         { return 4 * sizeof(float); }
        case Lvn_AttributeFormat_Vec2_f64:         { return 2 * sizeof(double); }
        case Lvn_AttributeFormat_Vec3_f64:         { return 3 * sizeof(double); }
        case Lvn_AttributeFormat_Vec4_f64:         { return 4 * sizeof(double); }
        case Lvn_AttributeFormat_Vec2_i32:         { return 2 * sizeof(int32_t); }
        case Lvn_AttributeFormat_Vec3_i32:         { return 3 * sizeof(int32_t); }
        case Lvn_AttributeFormat_Vec4_i32:         { return 4 * sizeof(int32_t); }
        case Lvn_AttributeFormat_Vec2_ui32:        { return 2 * sizeof(uint32_t); }
        case Lvn_AttributeFormat_Vec3_ui32:        { return 3 * sizeof(uint32_t); }
        case Lvn_AttributeFormat_Vec4_ui32:        { return 4 * sizeof(uint32_t); }
        case Lvn_AttributeFormat_Vec2_i8:          { return 2 * sizeof(int8_t); }
        case Lvn_AttributeFormat_Vec3_i8:          { return 3 * sizeof(int8_t); }
        case Lvn_AttributeFormat_Vec4_i8:          { return 4 * sizeof(int8_t); }
        case Lvn_AttributeFormat_Vec2_ui8:         { return 2 * sizeof(uint8_t); }
        case Lvn_AttributeFormat_Vec3_ui8:         { return 3 * sizeof(uint8_t); }
        case Lvn_AttributeFormat_Vec4_ui8:         { return 4 * sizeof(uint8_t); }
        case Lvn_AttributeFormat_Vec2_n8:          { return 2 * sizeof(int8_t); }
        case Lvn_AttributeFormat_Vec3_n8:          { return 3 * sizeof(int8_t); }
        case Lvn_AttributeFormat_Vec4_n8:          { return 4 * sizeof(int8_t); }
        case Lvn_AttributeFormat_Vec2_un8:         { return 2 * sizeof(uint8_t); }
        case Lvn_AttributeFormat_Vec3_un8:         { return 3 * sizeof(uint8_t); }
        case Lvn_AttributeFormat_Vec4_un8:         { return 4 * sizeof(uint8_t); }
        case Lvn_AttributeFormat_2_10_10_10_ile:   { return sizeof(int32_t); }
        case Lvn_AttributeFormat_2_10_10_10_uile:  { return sizeof(uint32_t); }
        case Lvn_AttributeFormat_2_10_10_10_nle:   { return sizeof(int32_t); }
        case Lvn_AttributeFormat_2_10_10_10_unle:  { return sizeof(uint32_t); }

        default:
        {
            LVN_CORE_WARN("unknown vertex data type enum: (%u)", format);
            return 0;
        }
    }
}

uint32_t getAttributeFormatComponentSize(LvnAttributeFormat format)
{
    switch (format)
    {
        case Lvn_AttributeFormat_Undefined:        { return 0; }
        case Lvn_AttributeFormat_Scalar_f32:       { return 1; }
        case Lvn_AttributeFormat_Scalar_f64:       { return 1; }
        case Lvn_AttributeFormat_Scalar_i32:       { return 1; }
        case Lvn_AttributeFormat_Scalar_ui32:      { return 1; }
        case Lvn_AttributeFormat_Scalar_i8:        { return 1; }
        case Lvn_AttributeFormat_Scalar_ui8:       { return 1; }
        case Lvn_AttributeFormat_Vec2_f32:         { return 2; }
        case Lvn_AttributeFormat_Vec3_f32:         { return 3; }
        case Lvn_AttributeFormat_Vec4_f32:         { return 4; }
        case Lvn_AttributeFormat_Vec2_f64:         { return 2; }
        case Lvn_AttributeFormat_Vec3_f64:         { return 3; }
        case Lvn_AttributeFormat_Vec4_f64:         { return 4; }
        case Lvn_AttributeFormat_Vec2_i32:         { return 2; }
        case Lvn_AttributeFormat_Vec3_i32:         { return 3; }
        case Lvn_AttributeFormat_Vec4_i32:         { return 4; }
        case Lvn_AttributeFormat_Vec2_ui32:        { return 2; }
        case Lvn_AttributeFormat_Vec3_ui32:        { return 3; }
        case Lvn_AttributeFormat_Vec4_ui32:        { return 4; }
        case Lvn_AttributeFormat_Vec2_i8:          { return 2; }
        case Lvn_AttributeFormat_Vec3_i8:          { return 3; }
        case Lvn_AttributeFormat_Vec4_i8:          { return 4; }
        case Lvn_AttributeFormat_Vec2_ui8:         { return 2; }
        case Lvn_AttributeFormat_Vec3_ui8:         { return 3; }
        case Lvn_AttributeFormat_Vec4_ui8:         { return 4; }
        case Lvn_AttributeFormat_Vec2_n8:          { return 2; }
        case Lvn_AttributeFormat_Vec3_n8:          { return 3; }
        case Lvn_AttributeFormat_Vec4_n8:          { return 4; }
        case Lvn_AttributeFormat_Vec2_un8:         { return 2; }
        case Lvn_AttributeFormat_Vec3_un8:         { return 3; }
        case Lvn_AttributeFormat_Vec4_un8:         { return 4; }
        case Lvn_AttributeFormat_2_10_10_10_ile:   { return 4; }
        case Lvn_AttributeFormat_2_10_10_10_uile:  { return 4; }
        case Lvn_AttributeFormat_2_10_10_10_nle:   { return 4; }
        case Lvn_AttributeFormat_2_10_10_10_unle:  { return 4; }

        default:
        {
            LVN_CORE_WARN("unknown vertex data type enum: (%u)", format);
            return 0;
        }
    }
}

bool isAttributeFormatNormalizedType(LvnAttributeFormat format)
{
    switch (format)
    {
        case Lvn_AttributeFormat_Vec2_n8:          { return true; }
        case Lvn_AttributeFormat_Vec3_n8:          { return true; }
        case Lvn_AttributeFormat_Vec4_n8:          { return true; }
        case Lvn_AttributeFormat_Vec2_un8:         { return true; }
        case Lvn_AttributeFormat_Vec3_un8:         { return true; }
        case Lvn_AttributeFormat_Vec4_un8:         { return true; }
        case Lvn_AttributeFormat_2_10_10_10_nle:   { return true; }
        case Lvn_AttributeFormat_2_10_10_10_unle:  { return true; }

        default: { return false; }
    }
}

void pipelineSpecificationSetConfig(LvnPipelineSpecification* pipelineSpecification)
{
    LVN_CORE_ASSERT(pipelineSpecification != nullptr, "pipeline specification points to nullptr when setting pipeline specification config");
    LvnContext* lvnctx = lvn::getContext();
    lvnctx->defaultPipelineSpecification = *pipelineSpecification;
}

LvnPipelineSpecification configPipelineSpecificationInit()
{
    LvnContext* lvnctx = lvn::getContext();
    return lvnctx->defaultPipelineSpecification;
}

void bufferUpdateData(LvnBuffer* buffer, void* data, uint64_t size, uint64_t offset)
{
    if (buffer->usage == Lvn_BufferUsage_Static)
    {
        LVN_CORE_ERROR("[opengl] cannot change data of buffer that has static buffer usage set Lvn_BufferUsage_Static, buffer: (%p)", buffer);
        return;
    }

    lvn::getContext()->graphicsContext.bufferUpdateData(buffer, data, size, offset);
}

void bufferResize(LvnBuffer* buffer, uint64_t size)
{
    if (buffer->usage != Lvn_BufferUsage_Resize)
    {
        LVN_CORE_ERROR("[opengl] cannot change data of buffer that does not have resize buffer usage set Lvn_BufferUsage_Resize, buffer: (%p)", buffer);
        return;
    }

    lvn::getContext()->graphicsContext.bufferResize(buffer, size);
}

LvnTexture* cubemapGetTextureData(LvnCubemap* cubemap)
{
    return &cubemap->textureData;
}

void updateDescriptorSetData(LvnDescriptorSet* descriptorSet, LvnDescriptorUpdateInfo* pUpdateInfo, uint32_t count)
{
    // TODO: add update error logs
    lvn::getContext()->graphicsContext.updateDescriptorSetData(descriptorSet, pUpdateInfo, count);
}

LvnTexture* frameBufferGetImage(LvnFrameBuffer* frameBuffer, uint32_t attachmentIndex)
{
    return lvn::getContext()->graphicsContext.frameBufferGetImage(frameBuffer, attachmentIndex);
}

LvnRenderPass* frameBufferGetRenderPass(LvnFrameBuffer* frameBuffer)
{
    return lvn::getContext()->graphicsContext.frameBufferGetRenderPass(frameBuffer);
}

void frameBufferResize(LvnFrameBuffer* frameBuffer, uint32_t width, uint32_t height)
{
    if (width * height == 0)
        return;

    lvn::getContext()->graphicsContext.framebufferResize(frameBuffer, width, height);
}

void frameBufferSetClearColor(LvnFrameBuffer* frameBuffer, uint32_t attachmentIndex, float r, float g, float b, float a)
{
    lvn::getContext()->graphicsContext.frameBufferSetClearColor(frameBuffer, attachmentIndex, r, g, b, a);
}

LvnDepthImageFormat findSupportedDepthImageFormat(LvnDepthImageFormat* pDepthImageFormats, uint32_t count)
{
    if (pDepthImageFormats == nullptr)
    {
        LVN_CORE_ERROR("cannot find supported depth image format, no depth image candidates given");
        return (LvnDepthImageFormat)(0);
    }

    return lvn::getContext()->graphicsContext.findSupportedDepthImageFormat(pDepthImageFormats, count);
}

LvnImageData loadImageData(const char* filepath, int forceChannels, bool flipVertically)
{
    if (filepath == nullptr)
    {
        LVN_CORE_ERROR("loadImageData(const char*, int, bool) | invalid filepath, filepath must not be nullptr");
        return {};
    }

    if (forceChannels < 0)
    {
        LVN_CORE_ERROR("loadImageData(const char*, int, bool) | forceChannels < 0, channels cannot be negative");
        return {};
    }
    else if (forceChannels > 4)
    {
        LVN_CORE_ERROR("loadImageData(const char*, int, bool) | forceChannels > 4, channels cannot be higher than 4 components (rgba)");
        return {};
    }

    stbi_set_flip_vertically_on_load(flipVertically);
    int imageWidth, imageHeight, imageChannels;
    stbi_uc* pixels = stbi_load(filepath, &imageWidth, &imageHeight, &imageChannels, forceChannels);

    if (!pixels)
    {
        LVN_CORE_ERROR("loadImageData(const char*, int, bool) | failed to load image pixel data from file: %s", filepath);
        return {};
    }

    LvnImageData imageData{};
    imageData.width = imageWidth;
    imageData.height = imageHeight;
    imageData.channels = forceChannels ? forceChannels : imageChannels;
    imageData.size = imageData.width * imageData.height * imageData.channels;
    imageData.pixels = LvnData<uint8_t>(pixels, imageData.size);

    LVN_CORE_TRACE("loaded image data <unsigned char*> (%p), (w:%u,h:%u,ch:%u), total memory size: %u bytes, filepath: %s", pixels, imageData.width, imageData.height, imageData.channels, imageData.size, filepath);

    stbi_image_free(pixels);

    return imageData;
}

LvnImageData loadImageDataMemory(const uint8_t* data, int length, int forceChannels, bool flipVertically)
{
    if (!data)
    {
        LVN_CORE_ERROR("loadImageDataMemory(const unsigned char*, int, int, bool) | invalid data, image memory data must not be nullptr");
        return {};
    }

    if (forceChannels < 0)
    {
        LVN_CORE_ERROR("loadImageDataMemory(conts unsigned char*, int, int, bool) | forceChannels < 0, channels cannot be negative");
        return {};
    }
    else if (forceChannels > 4)
    {
        LVN_CORE_ERROR("loadImageDataMemory(const unsigned char*, int, int, bool) | forceChannels > 4, channels cannot be higher than 4 components (rgba)");
        return {};
    }

    stbi_set_flip_vertically_on_load(flipVertically);
    int imageWidth, imageHeight, imageChannels;
    stbi_uc* pixels = stbi_load_from_memory(data, length, &imageWidth, &imageHeight, &imageChannels, forceChannels);

    if (!pixels)
    {
        LVN_CORE_ERROR("loadImageDataMemory(const unsigned char*) | failed to load image pixel data from memory: %p", data);
        return {};
    }

    LvnImageData imageData{};
    imageData.width = imageWidth;
    imageData.height = imageHeight;
    imageData.channels = forceChannels ? forceChannels : imageChannels;
    imageData.size = imageData.width * imageData.height * imageData.channels;
    imageData.pixels = LvnData<uint8_t>(pixels, imageData.size);

    LVN_CORE_TRACE("loaded image data from memory <unsigned char*> (%p), (w:%u,h:%u,ch:%u), total memory size: %u bytes", pixels, imageData.width, imageData.height, imageData.channels, imageData.size);

    stbi_image_free(pixels);

    return imageData;
}

LvnImageData loadImageDataThread(const LvnString filepath, int forceChannels, bool flipVertically)
{
    if (filepath.empty())
    {
        LVN_CORE_ERROR("loadImageDataThread(const char*, int, bool) | invalid filepath, filepath is empty string");
        return {};
    }

    if (forceChannels < 0)
    {
        LVN_CORE_ERROR("loadImageDataThread(const char*, int, bool) | forceChannels < 0, channels cannot be negative");
        return {};
    }
    else if (forceChannels > 4)
    {
        LVN_CORE_ERROR("loadImageDataThread(const char*, int, bool) | forceChannels > 4, channels cannot be higher than 4 components (rgba)");
        return {};
    }

    stbi_set_flip_vertically_on_load_thread(flipVertically);
    int imageWidth, imageHeight, imageChannels;
    stbi_uc* pixels = stbi_load(filepath.c_str(), &imageWidth, &imageHeight, &imageChannels, forceChannels);

    if (!pixels)
    {
        LVN_CORE_ERROR("loadImageDataThread(const char*, int, bool) | failed to load image pixel data from file: %s", filepath.c_str());
        return {};
    }

    LvnImageData imageData{};
    imageData.width = imageWidth;
    imageData.height = imageHeight;
    imageData.channels = forceChannels ? forceChannels : imageChannels;
    imageData.size = imageData.width * imageData.height * imageData.channels;
    imageData.pixels = LvnData<uint8_t>(pixels, imageData.size);

    LVN_CORE_TRACE("loaded image data <unsigned char*> (%p), (w:%u,h:%u,ch:%u), total memory size: %u bytes, filepath: %s", pixels, imageData.width, imageData.height, imageData.channels, imageData.size, filepath.c_str());

    stbi_image_free(pixels);

    return imageData;
}

LvnImageData loadImageDataMemoryThread(const uint8_t* data, int length, int forceChannels, bool flipVertically)
{
    if (!data)
    {
        LVN_CORE_ERROR("loadImageDataMemoryThread(const unsigned char*, int, int, bool) | invalid data, image memory data must not be nullptr");
        return {};
    }

    if (forceChannels < 0)
    {
        LVN_CORE_ERROR("loadImageDataMemoryThread(conts unsigned char*, int, int, bool) | forceChannels < 0, channels cannot be negative");
        return {};
    }
    else if (forceChannels > 4)
    {
        LVN_CORE_ERROR("loadImageDataMemoryThread(const unsigned char*, int, int, bool) | forceChannels > 4, channels cannot be higher than 4 components (rgba)");
        return {};
    }

    stbi_set_flip_vertically_on_load_thread(flipVertically);
    int imageWidth, imageHeight, imageChannels;
    stbi_uc* pixels = stbi_load_from_memory(data, length, &imageWidth, &imageHeight, &imageChannels, forceChannels);

    if (!pixels)
    {
        LVN_CORE_ERROR("loadImageDataMemoryThread(const unsigned char*) | failed to load image pixel data from memory: %p", data);
        return {};
    }

    LvnImageData imageData{};
    imageData.width = imageWidth;
    imageData.height = imageHeight;
    imageData.channels = forceChannels ? forceChannels : imageChannels;
    imageData.size = imageData.width * imageData.height * imageData.channels;
    imageData.pixels = LvnData<uint8_t>(pixels, imageData.size);

    LVN_CORE_TRACE("loaded image data from memory <unsigned char*> (%p), (w:%u,h:%u,ch:%u), total memory size: %u bytes", pixels, imageData.width, imageData.height, imageData.channels, imageData.size);

    stbi_image_free(pixels);

    return imageData;
}

LvnImageHdrData loadHdrImageData(const char* filepath, int forceChannels, bool flipVertically)
{
    if (filepath == nullptr)
    {
        LVN_CORE_ERROR("loadHdrImageData(const char*) | invalid filepath, filepath must not be nullptr");
        return {};
    }

    if (forceChannels < 0)
    {
        LVN_CORE_ERROR("loadHdrImageData(const char*) | forceChannels < 0, channels cannot be negative");
        return {};
    }
    else if (forceChannels > 4)
    {
        LVN_CORE_ERROR("loadHdrImageData(const char*) | forceChannels > 4, channels cannot be higher than 4 components (rgba)");
        return {};
    }

    stbi_set_flip_vertically_on_load(flipVertically);
    int imageWidth, imageHeight, imageChannels;
    float* pixels = stbi_loadf(filepath, &imageWidth, &imageHeight, &imageChannels, forceChannels);

    if (!pixels)
    {
        LVN_CORE_ERROR("loadHdrImageData(const char*) | failed to load image pixel data from file: %s", filepath);
        return {};
    }

    LvnImageHdrData imageData{};
    imageData.width = imageWidth;
    imageData.height = imageHeight;
    imageData.channels = forceChannels ? forceChannels : imageChannels;
    imageData.size = imageData.width * imageData.height * imageData.channels;
    imageData.pixels = LvnData<float>(pixels, imageData.size);

    LVN_CORE_TRACE("loaded hdr image data <float*> (%p), (w:%u,h:%u,ch:%u), total memory size: %u bytes, filepath: %s", pixels, imageData.width, imageData.height, imageData.channels, imageData.size, filepath);

    stbi_image_free(pixels);

    return imageData;
}

LvnResult writeImagePng(const LvnImageData& imageData, const char* filename)
{
    int stride = imageData.width * imageData.channels;
    int result = stbi_write_png(filename, (int)imageData.width, (int)imageData.height, (int)imageData.channels, imageData.pixels.data(), stride);
    return result ? Lvn_Result_Success : Lvn_Result_Failure;
}

LvnResult writeImageJpg(const LvnImageData& imageData, const char* filename, int quality)
{
    int result = stbi_write_jpg(filename, imageData.width, imageData.height, imageData.channels, imageData.pixels.data(), quality);
    return result ? Lvn_Result_Success : Lvn_Result_Failure;
}

LvnResult writeImageBmp(const LvnImageData& imageData, const char* filename)
{
    int result = stbi_write_bmp(filename, imageData.width, imageData.height, imageData.channels, imageData.pixels.data());
    return result ? Lvn_Result_Success : Lvn_Result_Failure;
}

void imageFlipVertically(LvnImageData& imageData)
{
    uint8_t* data = imageData.pixels.data();
    uint32_t rowSize = imageData.width * imageData.channels;
    LvnVector<uint8_t> tempRow(rowSize);

    for (uint32_t y = 0; y < imageData.height / 2; y++)
    {
        uint8_t* rowTop = data + y * rowSize;
        uint8_t* rowBottom = data + (imageData.height - y - 1) * rowSize;

        memcpy(tempRow.data(), rowTop, rowSize);
        memcpy(rowTop, rowBottom, rowSize);
        memcpy(rowBottom, tempRow.data(), rowSize);
    }
}

void imageFlipHorizontally(LvnImageData& imageData)
{
    uint8_t* data = imageData.pixels.data();

    for (uint32_t y = 0; y < imageData.height; y++)
    {
        uint8_t* row = data + y * imageData.width * imageData.channels;

        for (uint32_t x = 0; x < imageData.width / 2; x++)
        {
            uint8_t* leftpx = row + x * imageData.channels;
            uint8_t* rightpx = row + (imageData.width - x - 1) * imageData.channels;

            for (uint32_t c = 0; c < imageData.channels; c++)
                lvn::swap(leftpx[c], rightpx[c]);
        }
    }
}

void imageRotateCW(LvnImageData& imageData)
{
    uint8_t* data = imageData.pixels.data();
    uint32_t newWidth = imageData.height;
    uint32_t newHeight = imageData.width;

    LvnVector<uint8_t> rotated(newWidth * newHeight * imageData.channels);

    for (uint32_t y = 0; y < imageData.height; y++)
    {
        for (uint32_t x = 0; x < imageData.width; x++)
        {
            for (uint32_t c = 0; c < imageData.channels; c++)
            {
                uint32_t srcIndex = (y * imageData.width + x) * imageData.channels + c;
                uint32_t dstx = imageData.height - 1 - y;
                uint32_t dsty = x;
                uint32_t dstIndex = (dsty * newWidth + dstx) * imageData.channels + c;
                rotated[dstIndex] = data[srcIndex];
            }
        }
    }

    imageData.pixels = lvn::move(LvnData<uint8_t>(rotated.data(), rotated.size()));
    lvn::swap(imageData.width, imageData.height);
}

void imageRotateCCW(LvnImageData& imageData)
{
    uint8_t* data = imageData.pixels.data();
    uint32_t newWidth = imageData.height;
    uint32_t newHeight = imageData.width;

    LvnVector<uint8_t> rotated(newWidth * newHeight * imageData.channels);

    for (uint32_t y = 0; y < imageData.height; y++)
    {
        for (uint32_t x = 0; x < imageData.width; x++)
        {
            for (uint32_t c = 0; c < imageData.channels; c++)
            {
                uint32_t srcIndex = (y * imageData.width + x) * imageData.channels + c;
                uint32_t dstx = y;
                uint32_t dsty = imageData.width - 1 - x;
                uint32_t dstIndex = (dsty * newWidth + dstx) * imageData.channels + c;
                rotated[dstIndex] = data[srcIndex];
            }
        }
    }

    imageData.pixels = lvn::move(LvnData<uint8_t>(rotated.data(), rotated.size()));
    lvn::swap(imageData.width, imageData.height);
}

LvnImageData imageGenWhiteNoise(uint32_t width, uint32_t height, uint32_t channels)
{
    return lvn::imageGenWhiteNoise(width, height, channels, time(0));
}

LvnImageData imageGenWhiteNoise(uint32_t width, uint32_t height, uint32_t channels, uint32_t seed)
{
    LVN_CORE_ASSERT(channels > 0 && channels <= 4, "channels must be within 0 to 4");
    srand(seed);

    uint32_t imgSize = width * height * channels;
    uint8_t* imgBuff = (uint8_t*)LVN_MALLOC(imgSize);

    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            int rn = rand() % 2;
            for (uint32_t c = 0; c < channels; c++)
                imgBuff[y * width * channels + x * channels + c] = (c == 3 ? 255 : (rn ? 255 : 0));
        }
    }

    LvnImageData imageData{};
    imageData.width = width;
    imageData.height = height;
    imageData.channels = channels;
    imageData.size = width * height * channels;
    imageData.pixels = LvnData<uint8_t>(imgBuff, imgSize);

    LVN_FREE(imgBuff);
    return imageData;
}

LvnImageData imageGenGrayScaleNoise(uint32_t width, uint32_t height, uint32_t channels)
{
    return lvn::imageGenGrayScaleNoise(width, height, channels, time(0));
}

LvnImageData imageGenGrayScaleNoise(uint32_t width, uint32_t height, uint32_t channels, uint32_t seed)
{
    LVN_CORE_ASSERT(channels > 0 && channels <= 4, "channels must be within 0 to 4");
    srand(seed);

    uint32_t imgSize = width * height * channels;
    uint8_t* imgBuff = (uint8_t*)LVN_MALLOC(imgSize);

    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            int rn = rand() % 256;
            for (uint32_t c = 0; c < channels; c++)
                imgBuff[y * width * channels + x * channels + c] = (c == 3 ? 255 : rn);
        }
    }

    LvnImageData imageData{};
    imageData.width = width;
    imageData.height = height;
    imageData.channels = channels;
    imageData.size = width * height * channels;
    imageData.pixels = LvnData<uint8_t>(imgBuff, imgSize);

    LVN_FREE(imgBuff);
    return imageData;
}

LvnModel loadModel(const char* filepath)
{
    LvnString filepathstr(filepath);
    LvnString extensionType = filepathstr.substr(filepathstr.find_last_of(".") + 1);

    if (extensionType == "gltf")
    {
        return lvn::loadGltfModel(filepath);
    }
    else if (extensionType == "glb")
    {
        return lvn::loadGlbModel(filepath);
    }
    else if (extensionType == "obj")
    {
        return lvn::loadObjModel(filepath);
    }

    LVN_CORE_WARN("loadModel(const char*) | could not load model, file extension type not recognized (%s), Filepath: %s", extensionType.c_str(), filepath);
    return {};
}

void unloadModel(LvnModel* model)
{
    for (uint32_t i = 0; i < model->samplers.size(); i++)
    {
        lvn::destroySampler(model->samplers[i]);
    }
    for (uint32_t i = 0; i < model->textures.size(); i++)
    {
        lvn::destroyTexture(model->textures[i]);
    }
    for (uint32_t i = 0; i < model->buffers.size(); i++)
    {
        lvn::destroyBuffer(model->buffers[i]);
    }
    for (uint32_t i = 0; i < model->skins.size(); i++)
    {
        lvn::destroyBuffer(model->skins[i].ssbo);
    }
}


// ------------------------------------------------------------
// [SECTION]: Audio Functions
// ------------------------------------------------------------

float volumeDbToLinear(float db)
{
    return ma_volume_db_to_linear(db);
}

float volumeLineatToDb(float volume)
{
    return ma_volume_linear_to_db(volume);
}

void audioSetGlobalTimeMilliSeconds(uint64_t ms)
{
    ma_engine_set_time_in_milliseconds(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), ms);
}

void audioSetGlobalTimePcmFrames(uint64_t pcm)
{
    ma_engine_set_time_in_pcm_frames(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), pcm);
}

void audioSetMasterVolume(float volume)
{
    ma_engine_set_volume(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), volume);
}

uint32_t audioGetSampleRate()
{
    return ma_engine_get_sample_rate(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr));
}

uint64_t audioGetGlobalTimeMilliseconds()
{
    return ma_engine_get_time_in_milliseconds(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr));
}

uint64_t audioGetGlobalTimePcmFrames()
{
    return ma_engine_get_time_in_pcm_frames(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr));
}


void listenerSetPosition(float x, float y, float z)
{
    ma_engine_listener_set_position(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, x, y, z);
}

void listenerSetPosition(const LvnVec3& pos)
{
    ma_engine_listener_set_position(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, pos.x, pos.y, pos.z);
}

void listenerSetDirection(float x, float y, float z)
{
    ma_engine_listener_set_direction(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, x, y, z);
}

void listenerSetDirection(const LvnVec3 dir)
{
    ma_engine_listener_set_direction(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, dir.x, dir.y, dir.z);
}

void listenerSetVelocity(float x, float y, float z)
{
    ma_engine_listener_set_velocity(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, x, y, z);
}

void listenerSetVelocity(const LvnVec3 vel)
{
    ma_engine_listener_set_velocity(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, vel.x, vel.y, vel.z);
}

void listenerSetWorldUp(float x, float y, float z)
{
    ma_engine_listener_set_world_up(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, x, y, z);
}

void listenerSetWorldUp(const LvnVec3 up)
{
    ma_engine_listener_set_world_up(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, up.x, up.y, up.z);
}

void listenerSetCone(float innerAngleRad, float outerAngleRad, float outerGain)
{
    ma_engine_listener_set_cone(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, innerAngleRad, outerAngleRad, outerGain);
}

LvnVec3 listenerGetPosition()
{
    ma_vec3f pos = ma_engine_listener_get_position(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0);
    return LvnVec3{ pos.x, pos.y, pos.z };
}

LvnVec3 listenerGetDirection()
{
    ma_vec3f dir = ma_engine_listener_get_position(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0);
    return LvnVec3{ dir.x, dir.y, dir.z };
}

LvnVec3 listenerGetWorldUp()
{
    ma_vec3f up = ma_engine_listener_get_position(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0);
    return LvnVec3{ up.x, up.y, up.z };
}

void listenerGetCone(float* innerAngleRad, float* outerAngleRad, float* outerGain)
{
    ma_engine_listener_get_cone(static_cast<ma_engine*>(lvn::getContext()->audioEngineContextPtr), 0, innerAngleRad, outerAngleRad, outerGain);
}


LvnResult createSound(LvnSound** sound, const LvnSoundCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();
    ma_engine* pEngine = static_cast<ma_engine*>(lvnctx->audioEngineContextPtr);

    if (createInfo->filepath.empty())
    {
        LVN_CORE_ERROR("createSound(LvnSound**, LvnSoundCreateInfo*) | createInfo->filepath is nullptr, cannot load sound data without a valid path to the sound file");
        return Lvn_Result_Failure;
    }

    *sound = lvn::createObject<LvnSound>(lvnctx, Lvn_Stype_Sound);

    LvnSound* soundPtr = *sound;
    soundPtr->volume = createInfo->volume;
    soundPtr->pan = createInfo->pan;
    soundPtr->pitch = createInfo->pitch;
    soundPtr->pos = createInfo->pos;
    soundPtr->looping = createInfo->looping;

    ma_sound_config soundConfig{};

    if (ma_sound_init_from_file(pEngine, createInfo->filepath.c_str(), createInfo->flags, NULL, NULL, &soundPtr->sound) != MA_SUCCESS)
    {
        LVN_CORE_ERROR("createSound(LvnSound**, LvnSoundCreateInfo*) | failed to create sound object");
        return Lvn_Result_Failure;
    }

    ma_sound_set_volume(&soundPtr->sound, createInfo->volume);
    ma_sound_set_pan(&soundPtr->sound, createInfo->pan);
    ma_sound_set_pitch(&soundPtr->sound, createInfo->pitch);
    ma_sound_set_position(&soundPtr->sound, createInfo->pos.x, createInfo->pos.y, createInfo->pos.z);
    ma_sound_set_looping(&soundPtr->sound, createInfo->looping);

    LVN_CORE_TRACE("created sound: (%p), volume: %.2f, pan: %.2f, pitch: %.2f", *sound, createInfo->volume, createInfo->pan, createInfo->pitch);
    return Lvn_Result_Success;
}

void destroySound(LvnSound* sound)
{
    if (sound == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();

    ma_sound_uninit(&sound->sound);

    lvn::destroyObject(lvnctx, sound, Lvn_Stype_Sound);
}

LvnSoundCreateInfo configSoundInit(const char* filepath)
{
    LvnSoundCreateInfo soundInit{};
    soundInit.pos = { 0.0f, 0.0f, 0.0f };
    soundInit.volume = 1.0f;
    soundInit.pan = 0.0f;
    soundInit.pitch = 1.0f;
    soundInit.looping = false;
    soundInit.filepath = filepath;

    return soundInit;
}

void soundSetVolume(LvnSound* sound, float volume)
{
    ma_sound_set_volume(&sound->sound, volume);
}

void soundSetPan(LvnSound* sound, float pan)
{
    ma_sound_set_pan(&sound->sound, pan);
}

void soundSetPitch(LvnSound* sound, float pitch)
{
    ma_sound_set_pitch(&sound->sound, pitch);
}

void soundSetPositioning(LvnSound* sound, LvnSoundPositioningFlags positioning)
{
    ma_sound_set_positioning(&sound->sound, static_cast<ma_positioning>(positioning));
}

void soundSetPosition(LvnSound* sound, float x, float y, float z)
{
    ma_sound_set_position(&sound->sound, x, y, z);
}

void soundSetPosition(LvnSound* sound, const LvnVec3& pos)
{
    ma_sound_set_position(&sound->sound, pos.x, pos.y, pos.z);
}

void soundSetDirection(LvnSound* sound, float x, float y, float z)
{
    ma_sound_set_direction(&sound->sound, x, y, z);
}

void soundSetDirection(LvnSound* sound, const LvnVec3& dir)
{
    ma_sound_set_direction(&sound->sound, dir.x, dir.y, dir.z);
}

void soundSetVelocity(LvnSound* sound, float x, float y, float z)
{
    ma_sound_set_velocity(&sound->sound, x, y, z);
}

void soundSetVelocity(LvnSound* sound, const LvnVec3& vel)
{
    ma_sound_set_velocity(&sound->sound, vel.x, vel.y, vel.z);
}

void soundSetCone(LvnSound* sound, float innerAngleRad, float outerAngleRad, float outerGain)
{
    ma_sound_group_set_cone(&sound->sound, innerAngleRad, outerAngleRad, outerGain);
}

void soundSetAttenuation(LvnSound* sound, LvnSoundAttenuationFlags attenuation)
{
    ma_sound_set_attenuation_model(&sound->sound, static_cast<ma_attenuation_model>(attenuation));
}

void soundSetRolloff(LvnSound* sound, float rolloff)
{
    ma_sound_set_rolloff(&sound->sound, rolloff);
}

void soundSetMinGain(LvnSound* sound, float minGain)
{
    ma_sound_set_min_gain(&sound->sound, minGain);
}

void soundSetMaxGain(LvnSound* sound, float maxGain)
{
    ma_sound_set_max_gain(&sound->sound, maxGain);
}

void soundSetMinDistance(LvnSound* sound, float minDist)
{
    ma_sound_set_min_distance(&sound->sound, minDist);
}

void soundSetMaxDistance(LvnSound* sound, float maxDist)
{
    ma_sound_set_max_distance(&sound->sound, maxDist);
}

void soundSetDopplerFactor(LvnSound* sound, float dopplerFactor)
{
    ma_sound_set_doppler_factor(&sound->sound, dopplerFactor);
}

void soundSetLooping(LvnSound* sound, bool looping)
{
    ma_sound_set_looping(&sound->sound, looping);
}

void soundPlayStart(LvnSound* sound)
{
    ma_sound_start(&sound->sound);
}

void soundPlayStop(LvnSound* sound)
{
    ma_sound_stop(&sound->sound);
}

void soundTogglePause(LvnSound* sound)
{
    if (ma_sound_is_playing(&sound->sound))
        ma_sound_stop(&sound->sound);
    else
        ma_sound_start(&sound->sound);
}

LVN_API void soundScheduleStartTimePcmFrames(LvnSound* sound, uint64_t pcm)
{
    ma_sound_set_start_time_in_pcm_frames(&sound->sound, pcm);
}

void soundScheduleStartTimeMilliseconds(LvnSound* sound, uint64_t ms)
{
    ma_sound_set_start_time_in_milliseconds(&sound->sound, ms);
}

LVN_API void soundScheduleStopTimePcmFrames(LvnSound* sound, uint64_t pcm)
{
    ma_sound_set_stop_time_in_pcm_frames(&sound->sound, pcm);
}

void soundScheduleStopTimeMilliseconds(LvnSound* sound, uint64_t ms)
{
    ma_sound_set_stop_time_in_milliseconds(&sound->sound, ms);
}

void soundSetFadeMilliseconds(LvnSound* sound, float volBegin, float volEnd, uint64_t ms)
{
    ma_sound_set_fade_in_milliseconds(&sound->sound, volBegin, volEnd, ms);
}

void soundSetFadePcmFrames(LvnSound* sound, float volBegin, float volEnd, uint64_t pcm)
{
    ma_sound_set_fade_in_pcm_frames(&sound->sound, volBegin, volEnd, pcm);
}

void soundSeekToPcmFrame(LvnSound* sound, uint64_t pcm)
{
    ma_sound_seek_to_pcm_frame(&sound->sound, pcm);
}

float soundGetVolume(const LvnSound* sound)
{
    return ma_sound_get_volume(&sound->sound);
}

float soundGetPan(const LvnSound* sound)
{
    return ma_sound_get_pan(&sound->sound);
}

float soundGetPitch(const LvnSound* sound)
{
    return ma_sound_get_pitch(&sound->sound);
}

LvnSoundPositioningFlags soundGetPositioning(const LvnSound* sound)
{
    return static_cast<LvnSoundPositioningFlags>(ma_sound_get_positioning(&sound->sound));
}

LvnVec3 soundGetPosition(const LvnSound* sound)
{
    ma_vec3f pos = ma_sound_get_position(&sound->sound);
    return LvnVec3{ pos.x, pos.y, pos.z };
}

LvnVec3 soundGetDirection(const LvnSound* sound)
{
    ma_vec3f dir = ma_sound_get_direction(&sound->sound);
    return LvnVec3{ dir.x, dir.y, dir.z };
}

void soundGetCone(const LvnSound* sound, float* innerAngleRad, float* outerAngleRad, float* outerGain)
{
    ma_sound_get_cone(&sound->sound, innerAngleRad, outerAngleRad, outerGain);
}

LvnVec3 soundGetVelocity(const LvnSound* sound)
{
    ma_vec3f vel = ma_sound_get_velocity(&sound->sound);
    return LvnVec3{ vel.x, vel.y, vel.z };
}

LvnSoundAttenuationFlags soundGetAttenuation(const LvnSound* sound)
{
    return static_cast<LvnSoundAttenuationFlags>(ma_sound_get_attenuation_model(&sound->sound));
}

float soundGetRolloff(const LvnSound* sound)
{
    return ma_sound_get_rolloff(&sound->sound);
}

float soundGetMinGain(const LvnSound* sound)
{
    return ma_sound_get_min_gain(&sound->sound);
}

float soundGetMaxGain(const LvnSound* sound)
{
    return ma_sound_get_max_gain(&sound->sound);
}

float soundGetMinDistance(const LvnSound* sound)
{
    return ma_sound_get_min_distance(&sound->sound);
}

float soundGetMaxDistance(const LvnSound* sound)
{
    return ma_sound_get_max_distance(&sound->sound);
}

float soundGetDopplerFactor(const LvnSound* sound)
{
    return ma_sound_get_doppler_factor(&sound->sound);
}

bool soundIsLooping(const LvnSound* sound)
{
    return ma_sound_is_looping(&sound->sound);
}

bool soundIsPlaying(const LvnSound* sound)
{
    return ma_sound_is_playing(&sound->sound);
}

bool soundAtEnd(const LvnSound* sound)
{
    return ma_sound_at_end(&sound->sound);
}

uint64_t soundGetTimeMilliseconds(const LvnSound* sound)
{
    return ma_sound_get_time_in_milliseconds(&sound->sound);
}

uint64_t soundGetTimePcmFrames(const LvnSound* sound)
{
    return ma_sound_get_time_in_pcm_frames(&sound->sound);
}

float soundGetLengthSeconds(LvnSound* sound)
{
    float length;
    ma_sound_get_length_in_seconds(&sound->sound, &length);
    return length;
}

// ------------------------------------------------------------
// [SECTION]: Network Functions
// ------------------------------------------------------------

LvnResult createSocket(LvnSocket** socket, const LvnSocketCreateInfo* createInfo)
{
    LvnContext* lvnctx = lvn::getContext();

    *socket = lvn::createObject<LvnSocket>(lvnctx, Lvn_Stype_Socket);
    LvnSocket* socketPtr = *socket;

    ENetAddress address;
    address.host = createInfo->address.host;
    address.port = createInfo->address.port;

    if (createInfo->type == Lvn_SocketType_Client)
    {
        socketPtr->socket = enet_host_create(nullptr, createInfo->connectionCount, createInfo->channelCount, createInfo->inBandWidth, createInfo->outBandWidth);
    }
    else if (createInfo->type == Lvn_SocketType_Server)
    {
        socketPtr->socket = enet_host_create(&address, createInfo->connectionCount, createInfo->channelCount, createInfo->inBandWidth, createInfo->outBandWidth);
    }

    if (socketPtr->socket == nullptr)
    {
        LVN_CORE_ERROR("createSocket(LvnSocket**, LvnSocketCreateInfo*) | an error occured while trying to create socket");
        return Lvn_Result_Failure;
    }

    socketPtr->connection = nullptr;
    socketPtr->type = createInfo->type;
    socketPtr->address = createInfo->address;
    socketPtr->channelCount = createInfo->channelCount;
    socketPtr->channelCount = createInfo->channelCount;
    socketPtr->inBandWidth = createInfo->inBandWidth;
    socketPtr->outBandWidth = createInfo->outBandWidth;

    LVN_CORE_TRACE("created socket: (%p), address: (%u:%u)", createInfo->address.host, createInfo->address.port);
    return Lvn_Result_Success;
}

void destroySocket(LvnSocket* socket)
{
    if (socket == nullptr) { return; }
    LvnContext* lvnctx = lvn::getContext();
    enet_host_destroy(socket->socket);
    lvn::destroyObject(lvnctx, socket, Lvn_Stype_Socket);
}

LvnSocketCreateInfo configSocketClientInit(uint32_t connectionCount, uint32_t channelCount, uint32_t inBandwidth, uint32_t outBandWidth)
{
    LvnSocketCreateInfo createInfo{};
    createInfo.type = Lvn_SocketType_Client;
    createInfo.connectionCount = connectionCount;
    createInfo.channelCount = channelCount;
    createInfo.inBandWidth = inBandwidth;
    createInfo.outBandWidth = outBandWidth;

    return createInfo;
}

LvnSocketCreateInfo configSocketServerInit(LvnAddress address, uint32_t connectionCount, uint32_t channelCount, uint32_t inBandwidth, uint32_t outBandWidth)
{
    LvnSocketCreateInfo createInfo{};
    createInfo.type = Lvn_SocketType_Client;
    createInfo.address = address;
    createInfo.connectionCount = connectionCount;
    createInfo.channelCount = channelCount;
    createInfo.inBandWidth = inBandwidth;
    createInfo.outBandWidth = outBandWidth;

    return createInfo;
}

uint32_t socketGetHostFromStr(const char* host)
{
    ENetAddress address;
    enet_address_set_host(&address, host);
    return address.host;
}

LvnResult socketConnect(LvnSocket* socket, LvnAddress* address, uint32_t channelCount, uint32_t milliseconds)
{
    if (socket->type != Lvn_SocketType_Client)
    {
        LVN_CORE_ERROR("cannot use socket (%p) with type that is not client to connect", socket->socket);
        return Lvn_Result_Failure;
    }

    ENetAddress enetAddress;
    enetAddress.host = socket->address.host;
    enetAddress.port = socket->address.port;

    socket->connection = enet_host_connect(socket->socket, &enetAddress, channelCount, 0);

    if (socket->connection == nullptr)
    {
        LVN_CORE_ERROR("no available peers for initiating a connection on socket (%p)", socket);
        return Lvn_Result_Failure;
    }

    ENetEvent event;
    if (enet_host_service(socket->socket, &event, milliseconds) > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
    {
        return Lvn_Result_Success;
    }

    enet_peer_reset(socket->connection);
    return Lvn_Result_TimeOut;
}

LvnResult socketDisconnect(LvnSocket* socket, uint32_t milliseconds)
{
    if (socket->type != Lvn_SocketType_Client)
    {
        LVN_CORE_ERROR("cannot use socket (%p) with type that is not client to disconnect", socket->socket);
        return Lvn_Result_Failure;
    }
    enet_peer_disconnect(socket->connection, 0);

    ENetEvent event;
    if (enet_host_service(socket->socket, &event, milliseconds) > 0)
    {
        switch (event.type)
        {
            case ENET_EVENT_TYPE_RECEIVE:
            {
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
            {
                return Lvn_Result_Success;
            }

            default:
            {
                LVN_CORE_WARN("unknown disconnect event received on socket (%p)", socket);
                break;
            }
        }
    }

    enet_peer_reset(socket->connection);
    return Lvn_Result_Success;
}

void socketSend(LvnSocket* socket, uint8_t channel, LvnPacket* packet)
{
    LVN_CORE_ASSERT(packet != nullptr, "packet is nullptr when trying to send packet through socket");

    ENetPacket* enetPacket = enet_packet_create(packet->data, packet->size, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(socket->connection, channel, enetPacket);
    enet_host_flush(socket->socket);
}

LvnResult socketReceive(LvnSocket* socket, LvnPacket* packet, uint32_t milliseconds)
{
    LVN_CORE_ASSERT(packet != nullptr, "packet is nullptr when trying to receive packet from socket");

    ENetEvent event;
    if (enet_host_service(socket->socket, &event, milliseconds) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE)
    {
        packet->data = event.packet->data;
        packet->size = event.packet->dataLength;
        enet_packet_destroy(event.packet);
        return Lvn_Result_Success;
    }

    return Lvn_Result_TimeOut;
}

// ------------------------------------------------------------
// [SECTION]: Math Functions
// ------------------------------------------------------------

float radians(float deg)
{
    return deg * 0.0174532925199f; // deg * (PI / 180)
}

float degrees(float rad)
{
    return rad * 57.2957795131f; // rad * (180 / PI)
}

float clampAngle(float rad)
{
    float angle = fmod(rad, 2 * LVN_PI);
    if (angle < 0) { angle += 2 * LVN_PI; }
    return angle;
}

float clampAngleDeg(float deg)
{
    float angle = fmod(deg, 360.0f);
    if (angle < 0) { angle += 360.0f; }
    return angle;
}

float invSqrt(float num)
{
    union
    {
        float f;
        uint32_t i;
    } conv;

    float x2;
    const float threehalfs = 1.5f;

    x2 = num * 0.5f;
    conv.f = num;
    conv.i = 0x5f3759df - (conv.i >> 1);
    conv.f = conv.f * (threehalfs - (x2 * conv.f * conv.f));
    return conv.f;
}

double derivative(double (*func)(double), double x, double delta)
{
    double fxph = func(x + delta);
    double fxmh = func(x - delta);
    return (fxph - fxmh) / (2.0 * delta);
}

} /* namespace lvn */
