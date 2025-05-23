#include <levikno/levikno.h>


#define ARRAY_LEN(x) (sizeof(x) / sizeof(x[0]))

// INFO: this program demonstrates different ways to load shaders, shaders can be loaded three ways:
//       - shaders can be loaded directly from source
//       - shaders can be loaded from an external source file (eg .glsl, .vert, .frag)
//       - shaders can be loaded from an external binary file (eg .spv)
// NOTE: shaders loaded from an external file will take the filepath to the shader file instead of the source code

// NOTE: for loading shaders from binary files, you will need to generate the binary files yourself (using glslang or shaderc)
//       - use the shader files from the examples folder for generating the spirv binaries
//       - replace the shader module parameters in LvnShaderCreateInfo with the filepaths to the binary files
//       - replace the shader create function to lvn::createShaderFromFileBin()

static float s_Vertices[] =
{
/*      Pos (x,y,z)   |   color (r,g,b)   */
     0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, // v1
    -0.5f,-0.5f, 0.0f, 0.0f, 1.0f, 0.0f, // v2
     0.5f,-0.5f, 0.0f, 0.0f, 0.0f, 1.0f, // v3
};

static uint32_t s_Indices[] = 
{
    0, 1, 2
};

static const char* s_VertexShaderSrc = R"(
#version 460

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main()
{
    gl_Position = vec4(inPos, 1.0);
    fragColor = inColor;
}
)";

static const char* s_FragmentShaderSrc = R"(
#version 460

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec3 fragColor;

void main()
{
    outColor = vec4(fragColor, 1.0);
}
)";


int main(int argc, char** argv)
{
    // [Create Context]
    // create the context to load the library

    LvnContextCreateInfo lvnCreateInfo{};
    lvnCreateInfo.logging.enableLogging = true;
    lvnCreateInfo.logging.enableGraphicsApiDebugLogs = true;
    lvnCreateInfo.windowapi = Lvn_WindowApi_glfw;
    lvnCreateInfo.graphicsapi = Lvn_GraphicsApi_vulkan;

    lvn::createContext(&lvnCreateInfo);


    // window create info struct
    LvnWindowCreateInfo windowInfo{};
    windowInfo.title = "shaderLoading";
    windowInfo.width = 800;
    windowInfo.height = 600;
    windowInfo.minWidth = 300;
    windowInfo.minHeight = 200;

    LvnWindow* window;
    lvn::createWindow(&window, &windowInfo);


    // [Create Buffer]
    // create the buffer to store our vertex data

    // create the vertex attributes and descriptor bindings to layout our vertex data
    LvnVertexAttribute attributes[2] =
    {
        { 0, 0, Lvn_AttributeFormat_Vec3_f32, 0 },
        { 0, 1, Lvn_AttributeFormat_Vec3_f32, (3 * sizeof(float)) },
    };

    LvnVertexBindingDescription vertexBindingDescription{};
    vertexBindingDescription.binding = 0;
    vertexBindingDescription.stride = 6 * sizeof(float);

    // vertex buffer create info struct
    LvnBufferCreateInfo bufferCreateInfo{};
    bufferCreateInfo.type = Lvn_BufferType_Vertex;
    bufferCreateInfo.usage = Lvn_BufferUsage_Static;
    bufferCreateInfo.data = s_Vertices;
    bufferCreateInfo.size = sizeof(s_Vertices);

    // create buffer
    LvnBuffer* vertexBuffer;
    lvn::createBuffer(&vertexBuffer, &bufferCreateInfo);

    // index buffer create info struct
    bufferCreateInfo.type = Lvn_BufferType_Index;
    bufferCreateInfo.usage = Lvn_BufferUsage_Static;
    bufferCreateInfo.data = s_Indices;
    bufferCreateInfo.size = sizeof(s_Indices);

    // create buffer
    LvnBuffer* indexBuffer;
    lvn::createBuffer(&indexBuffer, &bufferCreateInfo);


    // [Create Pipeline]
    // create the pipeline for how we want to render our scene

    // load shader from source
    // shader create info struct
    LvnShaderCreateInfo shaderCreateInfo{};               // NOTE: shader source code is directly inputed as string
    shaderCreateInfo.vertexSrc = s_VertexShaderSrc;
    shaderCreateInfo.fragmentSrc = s_FragmentShaderSrc;

    // create shader from source
    LvnShader* shader;
    lvn::createShaderFromSrc(&shader, &shaderCreateInfo); // NOTE: shader is loaded directly from source, the shader source code is taken in as parameter


    // load shader from source file
    LvnShaderCreateInfo shaderFileCreateInfo{};                           // NOTE: filepath to shader source file is taken in as string
    shaderFileCreateInfo.vertexSrc = "res/shaders/shaderLoading.vert";
    shaderFileCreateInfo.fragmentSrc = "res/shaders/shaderLoading.frag";

    // create shader from source
    LvnShader* shaderFile;
    lvn::createShaderFromFileSrc(&shaderFile, &shaderFileCreateInfo);     // NOTE: shader is loaded from external file source, the shader filepath is taken in as parameter


    // load shader from binary file
    LvnShaderCreateInfo shaderBinCreateInfo{};
    shaderBinCreateInfo.vertexSrc = "res/shaders/shaderLoadingVert.spv";
    shaderBinCreateInfo.fragmentSrc = "res/shaders/shaderLoadingFrag.spv";

    // create shader from source
    LvnShader* shaderBin;
    lvn::createShaderFromFileBin(&shaderBin, &shaderBinCreateInfo);

    // get the render pass from the window to pass into the pipeline
    LvnRenderPass* renderPass = lvn::windowGetRenderPass(window);

    // create pipeline specification or fixed functions
    LvnPipelineSpecification pipelineSpec = lvn::configPipelineSpecificationInit();

    // pipeline create info struct
    LvnPipelineCreateInfo pipelineCreateInfo{};
    pipelineCreateInfo.pipelineSpecification = &pipelineSpec;
    pipelineCreateInfo.pVertexAttributes = attributes;
    pipelineCreateInfo.vertexAttributeCount = 2;
    pipelineCreateInfo.pVertexBindingDescriptions = &vertexBindingDescription;
    pipelineCreateInfo.vertexBindingDescriptionCount = 1;
    pipelineCreateInfo.shader = shader;  // INFO: switch between shader, shaderFile, or shaderBin when passing into the pipeline
    pipelineCreateInfo.renderPass = renderPass;

    // create pipeline
    LvnPipeline* pipeline;
    lvn::createPipeline(&pipeline, &pipelineCreateInfo);

    // destroy the shader after creating the pipeline
    lvn::destroyShader(shader);
    lvn::destroyShader(shaderFile);
    lvn::destroyShader(shaderBin);


    // [Main Render Loop]
    while (lvn::windowOpen(window))
    {
        lvn::windowUpdate(window);
        lvn::windowPollEvents();

        // get next window swapchain image
        lvn::renderBeginNextFrame(window);
        lvn::renderBeginCommandRecording(window);

        // set background color and begin render pass
        lvn::renderCmdBeginRenderPass(window, 0.0f, 0.0f, 0.0f, 1.0f);

        // bind pipeline
        lvn::renderCmdBindPipeline(window, pipeline);

        // bind vertex and index buffer
        lvn::renderCmdBindVertexBuffer(window, 0, 1, &vertexBuffer, 0);
        lvn::renderCmdBindIndexBuffer(window, indexBuffer, 0);

        // draw triangle
        lvn::renderCmdDrawIndexed(window, ARRAY_LEN(s_Indices)); // number of elements in indices array (3)

        // end render pass and submit rendering
        lvn::renderCmdEndRenderPass(window);
        lvn::renderEndCommandRecording(window);
        lvn::renderDrawSubmit(window); // note that this function is where we actually submit our render data to the GPU
    }

    // destroy objects after they are finished being used
    lvn::destroyBuffer(vertexBuffer);
    lvn::destroyBuffer(indexBuffer);
    lvn::destroyPipeline(pipeline);
    lvn::destroyWindow(window);

    // terminate the context at the end of the program
    lvn::terminateContext();

    return 0;
}
