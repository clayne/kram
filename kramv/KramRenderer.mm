// kram - Copyright 2020 by Alec Miller. - MIT License
// The license and copyright notice shall be included
// in all copies or substantial portions of the Software.

#import "KramRenderer.h"

#import <TargetConditionals.h>

#if __has_feature(modules)
@import simd;
@import ModelIO;
#else
#import <simd/simd.h>
#import <ModelIO/ModelIO.h>
#endif

// Include header shared between C code here, which executes Metal API commands, and .metal files
#import "KramShaders.h"
#import "KramLoader.h"

#include "KTXImage.h"
#include "Kram.h"

static const NSUInteger MaxBuffersInFlight = 3;

ShowSettings gShowSettings;

using namespace kram;
using namespace simd;

@implementation Renderer
{
    dispatch_semaphore_t _inFlightSemaphore;
    id <MTLDevice> _device;
    id <MTLCommandQueue> _commandQueue;

    id <MTLBuffer> _dynamicUniformBuffer[MaxBuffersInFlight];
    
    id <MTLRenderPipelineState> _pipelineState1DArray;
    id <MTLRenderPipelineState> _pipelineStateImage;
    id <MTLRenderPipelineState> _pipelineStateImageArray;
    id <MTLRenderPipelineState> _pipelineStateCube;
    id <MTLRenderPipelineState> _pipelineStateCubeArray;
    id <MTLRenderPipelineState> _pipelineStateVolume;
    
    id <MTLComputePipelineState> _pipelineState1DArrayCS;
    id <MTLComputePipelineState> _pipelineStateImageCS;
    id <MTLComputePipelineState> _pipelineStateImageArrayCS;
    id <MTLComputePipelineState> _pipelineStateCubeCS;
    id <MTLComputePipelineState> _pipelineStateCubeArrayCS;
    id <MTLComputePipelineState> _pipelineStateVolumeCS;
    
    id <MTLDepthStencilState> _depthStateFull;
    id <MTLDepthStencilState> _depthStateNone;
   
    MTLVertexDescriptor *_mtlVertexDescriptor;

    // TODO: Array< id<MTLTexture> > _textures;
    id <MTLTexture> _colorMap;
    //id <MTLTexture> _colorMapView;
    
    id <MTLSamplerState> _colorMapSamplerWrap;
    id <MTLSamplerState> _colorMapSamplerClamp;
    
    id <MTLSamplerState> _colorMapSamplerBilinearWrap;
    id <MTLSamplerState> _colorMapSamplerBilinearClamp;
    
    //id<MTLTexture> _sampleRT;
    id<MTLTexture> _sampleTex;
    
    uint8_t _uniformBufferIndex;

    float4x4 _projectionMatrix;
    float4x4 _viewMatrix;
    float4x4 _modelMatrix;

    //float _rotation;
    KramLoader *_loader;
    MTKMesh *_mesh;
    
    string _lastFilename;
    double _lastTimestamp;
}

-(nonnull instancetype)initWithMetalKitView:(nonnull MTKView *)view
{
    self = [super init];
    if(self)
    {
        _device = view.device;
        _lastTimestamp = 0.0;
        
        _loader = [KramLoader new];
        _loader.device = _device;
        
        _inFlightSemaphore = dispatch_semaphore_create(MaxBuffersInFlight);
        [self _loadMetalWithView:view];
        [self _loadAssets];
    }

    return self;
}

- (void)_createSamplers
{
    MTLSamplerDescriptor *samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.mipFilter = MTLSamplerMipFilterNearest;
    
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeRepeat;
    samplerDescriptor.rAddressMode = MTLSamplerAddressModeRepeat;
    samplerDescriptor.label = @"colorMapSamplerWrap";
    
    _colorMapSamplerWrap = [_device newSamplerStateWithDescriptor:samplerDescriptor];
    
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToBorderColor;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToBorderColor;
    samplerDescriptor.rAddressMode = MTLSamplerAddressModeClampToBorderColor;
    samplerDescriptor.label = @"colorMapSamplerClamp";
   
    _colorMapSamplerClamp = [_device newSamplerStateWithDescriptor:samplerDescriptor];
    
    // these are for preview mode
    // use the mips, and specify linear for min/mag for SDF case
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
    samplerDescriptor.mipFilter = MTLSamplerMipFilterLinear;
    samplerDescriptor.label = @"colorMapSamplerBilinearClamp";
   
    _colorMapSamplerBilinearClamp = [_device newSamplerStateWithDescriptor:samplerDescriptor];
    
    samplerDescriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    samplerDescriptor.tAddressMode = MTLSamplerAddressModeRepeat;
    samplerDescriptor.rAddressMode = MTLSamplerAddressModeRepeat;
    samplerDescriptor.label = @"colorMapSamplerBilinearWrap";
    
    _colorMapSamplerBilinearWrap = [_device newSamplerStateWithDescriptor:samplerDescriptor];
}
    
- (void)_loadMetalWithView:(nonnull MTKView *)view
{
    /// Load Metal state objects and initialize renderer dependent view properties

    view.depthStencilPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm_sRGB; // TODO: adjust this to draw srgb or not, prefer RGBA
    view.sampleCount = 1;

    _mtlVertexDescriptor = [[MTLVertexDescriptor alloc] init];

    _mtlVertexDescriptor.attributes[VertexAttributePosition].format = MTLVertexFormatFloat3;
    _mtlVertexDescriptor.attributes[VertexAttributePosition].offset = 0;
    _mtlVertexDescriptor.attributes[VertexAttributePosition].bufferIndex = BufferIndexMeshPositions;

    _mtlVertexDescriptor.attributes[VertexAttributeTexcoord].format = MTLVertexFormatFloat2;
    _mtlVertexDescriptor.attributes[VertexAttributeTexcoord].offset = 0;
    _mtlVertexDescriptor.attributes[VertexAttributeTexcoord].bufferIndex = BufferIndexMeshGenerics;

    _mtlVertexDescriptor.layouts[BufferIndexMeshPositions].stride = 12;
    //_mtlVertexDescriptor.layouts[BufferIndexMeshPositions].stepRate = 1;
    //_mtlVertexDescriptor.layouts[BufferIndexMeshPositions].stepFunction = MTLVertexStepFunctionPerVertex;

    _mtlVertexDescriptor.layouts[BufferIndexMeshGenerics].stride = 8;
    //_mtlVertexDescriptor.layouts[BufferIndexMeshGenerics].stepRate = 1;
    //_mtlVertexDescriptor.layouts[BufferIndexMeshGenerics].stepFunction = MTLVertexStepFunctionPerVertex;

    [self _createRenderPipelines:view];
    
    //-----------------------
   
    MTLDepthStencilDescriptor *depthStateDesc = [[MTLDepthStencilDescriptor alloc] init];
    depthStateDesc.depthCompareFunction = gShowSettings.isReverseZ ? MTLCompareFunctionGreaterEqual : MTLCompareFunctionLessEqual;
    depthStateDesc.depthWriteEnabled = YES;
    _depthStateFull = [_device newDepthStencilStateWithDescriptor:depthStateDesc];

    depthStateDesc.depthCompareFunction = gShowSettings.isReverseZ ? MTLCompareFunctionGreaterEqual : MTLCompareFunctionLessEqual;
    depthStateDesc.depthWriteEnabled = NO;
    _depthStateNone = [_device newDepthStencilStateWithDescriptor:depthStateDesc];
    
    for(NSUInteger i = 0; i < MaxBuffersInFlight; i++)
    {
        _dynamicUniformBuffer[i] = [_device newBufferWithLength:sizeof(Uniforms)
                                                        options:MTLResourceStorageModeShared];

        _dynamicUniformBuffer[i].label = @"UniformBuffer";
    }

    _commandQueue = [_device newCommandQueue];
    
    [self _createSamplers];
    
    //-----------------------
   
    [self _createComputePipelines];
   
    [self _createSampleRender];
}

- (void)_createComputePipelines
{
    id<MTLLibrary> defaultLibrary = [_device newDefaultLibrary];

    NSError *error = NULL;
    id<MTLFunction> computeFunction;
    
    //-----------------------
   
    computeFunction = [defaultLibrary newFunctionWithName:@"SampleImageCS"];
    _pipelineStateImageCS = [_device newComputePipelineStateWithFunction:computeFunction error:&error];
    if (!_pipelineStateImageCS)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
    
    computeFunction = [defaultLibrary newFunctionWithName:@"SampleImageArrayCS"];
    _pipelineStateImageArrayCS = [_device newComputePipelineStateWithFunction:computeFunction error:&error];
    if (!_pipelineStateImageArrayCS)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
    
    computeFunction = [defaultLibrary newFunctionWithName:@"SampleVolumeCS"];
    _pipelineStateVolumeCS = [_device newComputePipelineStateWithFunction:computeFunction error:&error];
    if (!_pipelineStateVolumeCS)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
    
    computeFunction = [defaultLibrary newFunctionWithName:@"SampleCubeCS"];
    _pipelineStateCubeCS = [_device newComputePipelineStateWithFunction:computeFunction error:&error];
    if (!_pipelineStateCubeCS)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
    
    computeFunction = [defaultLibrary newFunctionWithName:@"SampleCubeArrayCS"];
    _pipelineStateCubeArrayCS = [_device newComputePipelineStateWithFunction:computeFunction error:&error];
    if (!_pipelineStateCubeArrayCS)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
    
    computeFunction = [defaultLibrary newFunctionWithName:@"SampleImage1DArrayCS"];
    _pipelineState1DArrayCS = [_device newComputePipelineStateWithFunction:computeFunction error:&error];
    if (!_pipelineState1DArrayCS)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
}

- (void)_createRenderPipelines:(MTKView*)view
{
    id<MTLLibrary> defaultLibrary = [_device newDefaultLibrary];

    id <MTLFunction> vertexFunction;
    id <MTLFunction> fragmentFunction;
    
    MTLRenderPipelineDescriptor *pipelineStateDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineStateDescriptor.label = @"DrawImagePipeline";
    pipelineStateDescriptor.sampleCount = view.sampleCount;
    pipelineStateDescriptor.vertexDescriptor = _mtlVertexDescriptor;
    pipelineStateDescriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat;
    
    // TODO: could drop these for images, but want a 3D preview of content
    // or might make these memoryless.
    pipelineStateDescriptor.depthAttachmentPixelFormat = view.depthStencilPixelFormat;
    pipelineStateDescriptor.stencilAttachmentPixelFormat = view.depthStencilPixelFormat;

    NSError *error = NULL;
    
    //-----------------------
   
    vertexFunction = [defaultLibrary newFunctionWithName:@"DrawImageVS"];
    fragmentFunction = [defaultLibrary newFunctionWithName:@"DrawImagePS"];
    pipelineStateDescriptor.vertexFunction = vertexFunction;
    pipelineStateDescriptor.fragmentFunction = fragmentFunction;
    
    _pipelineStateImage = [_device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor error:&error];
    if (!_pipelineStateImage)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }

    //-----------------------
   
    vertexFunction = [defaultLibrary newFunctionWithName:@"DrawImageVS"]; // reused
    fragmentFunction = [defaultLibrary newFunctionWithName:@"DrawImageArrayPS"];
    pipelineStateDescriptor.vertexFunction = vertexFunction;
    pipelineStateDescriptor.fragmentFunction = fragmentFunction;
    
    _pipelineStateImageArray = [_device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor error:&error];
    if (!_pipelineStateImageArray)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }

    //-----------------------
   
    vertexFunction = [defaultLibrary newFunctionWithName:@"DrawImageVS"];
    fragmentFunction = [defaultLibrary newFunctionWithName:@"Draw1DArrayPS"];
    pipelineStateDescriptor.vertexFunction = vertexFunction;
    pipelineStateDescriptor.fragmentFunction = fragmentFunction;
    
    _pipelineState1DArray = [_device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor error:&error];
    if (!_pipelineState1DArray)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
    
    //-----------------------
   
    vertexFunction = [defaultLibrary newFunctionWithName:@"DrawCubeVS"];
    fragmentFunction = [defaultLibrary newFunctionWithName:@"DrawCubePS"];
    pipelineStateDescriptor.vertexFunction = vertexFunction;
    pipelineStateDescriptor.fragmentFunction = fragmentFunction;
    
    _pipelineStateCube = [_device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor error:&error];
    if (!_pipelineStateCube)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
    
    //-----------------------
   
    vertexFunction = [defaultLibrary newFunctionWithName:@"DrawCubeVS"]; // reused
    fragmentFunction = [defaultLibrary newFunctionWithName:@"DrawCubeArrayPS"];
    pipelineStateDescriptor.vertexFunction = vertexFunction;
    pipelineStateDescriptor.fragmentFunction = fragmentFunction;
    
    _pipelineStateCubeArray = [_device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor error:&error];
    if (!_pipelineStateCubeArray)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
    
    //-----------------------
    
    vertexFunction = [defaultLibrary newFunctionWithName:@"DrawVolumeVS"];
    fragmentFunction = [defaultLibrary newFunctionWithName:@"DrawVolumePS"];
    pipelineStateDescriptor.vertexFunction = vertexFunction;
    pipelineStateDescriptor.fragmentFunction = fragmentFunction;
    
    _pipelineStateVolume = [_device newRenderPipelineStateWithDescriptor:pipelineStateDescriptor error:&error];
    if (!_pipelineStateVolume)
    {
        NSLog(@"Failed to create pipeline state, error %@", error);
    }
}

- (void)_createSampleRender
{
    // writing to this texture
    MTLTextureDescriptor* textureDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    
    textureDesc.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    textureDesc.storageMode = MTLStorageModeManaged;
    _sampleTex = [_device newTextureWithDescriptor:textureDesc];
}

- (void)_loadAssets
{
    /// Load assets into metal objects

    NSError *error;

    MTKMeshBufferAllocator *metalAllocator = [[MTKMeshBufferAllocator alloc]
                                              initWithDevice: _device];

#if 1 // TODO: replace box with fsq or fst, or use thin box for perspective/rotation
    MDLMesh *mdlMesh = [MDLMesh newBoxWithDimensions:(vector_float3){1, 1, 1}
                                            segments:(vector_uint3){1, 1, 1}
                                        geometryType:MDLGeometryTypeTriangles
                                       inwardNormals:NO
                                           allocator:metalAllocator];
    
#endif
    
    MDLVertexDescriptor *mdlVertexDescriptor =
    MTKModelIOVertexDescriptorFromMetal(_mtlVertexDescriptor);

    mdlVertexDescriptor.attributes[VertexAttributePosition].name  = MDLVertexAttributePosition;
    mdlVertexDescriptor.attributes[VertexAttributeTexcoord].name  = MDLVertexAttributeTextureCoordinate;

    mdlMesh.vertexDescriptor = mdlVertexDescriptor;

    _mesh = [[MTKMesh alloc] initWithMesh:mdlMesh
                                   device:_device
                                    error:&error];
    _mesh.name = @"BoxMesh";
    
    if(!_mesh || error)
    {
        NSLog(@"Error creating MetalKit mesh %@", error.localizedDescription);
    }
}



- (BOOL)loadTexture:(nonnull NSURL *)url
{
    string fullFilename = [url.path UTF8String];
    
    // can use this to pull, or use fstat on FileHelper
    NSDate *fileDate = nil;
    NSError *error = nil;
    [url getResourceValue:&fileDate forKey:NSURLContentModificationDateKey error:&error];
    
    // DONE: tie this to url and modstamp differences
    double timestamp = fileDate.timeIntervalSince1970;
    bool isTextureChanged = (fullFilename != _lastFilename) || (timestamp != _lastTimestamp);
    
    // image can be decoded to rgba8u if platform can't display format natively
    // but still want to identify blockSize from original format
    MyMTLPixelFormat format;
    MyMTLPixelFormat originalFormat = (MyMTLPixelFormat)gShowSettings.originalFormat;
    
    id<MTLTexture> texture;
    
    if (isTextureChanged) {
        // synchronously cpu upload from ktx file to texture
        MTLPixelFormat originalFormatMTL = MTLPixelFormatInvalid;
        texture = [_loader loadTextureFromURL:url originalFormat:&originalFormatMTL];
        if (!texture) {
            return NO;
        }
        
        _colorMap = texture;
        
        format = (MyMTLPixelFormat)texture.pixelFormat;
        originalFormat = (MyMTLPixelFormat)originalFormatMTL;
        gShowSettings.originalFormat = originalFormatMTL;
        
        _lastFilename = fullFilename;
        _lastTimestamp = timestamp;
        
        bool isVerbose = false;
        gShowSettings.imageInfo = kramInfoToString(fullFilename, isVerbose);
        
        // use MTLView to handle toggle of srgb read state (writes are still lin -> srgb framebuffer)
        //_colorMapView = nil;
       
        // if we guess wrong on srgb, then have ability to see without srgb reads
        // TODO: this only works for sRGBA8 <-> RGBA8, not BC1_SRGB <-> BC1 and other encode formats
        // require that MTLTextureUsagePixelFormatView be set on the originally created texture.  Ugh.
//        bool isSrgbFormat_ = isSrgbFormat(format);
//        if (isSrgbFormat_) {
//            MyMTLPixelFormat fmtNoSRGB = toggleSrgbFormat(format);
//            _colorMapView = [_colorMap newTextureViewWithPixelFormat:(MTLPixelFormat)fmtNoSRGB];
//        }
        
        Int2 blockDims = blockDimsOfFormat(originalFormat);
        gShowSettings.blockX = blockDims.x;
        gShowSettings.blockY = blockDims.y;
    }
    
    texture = _colorMap;
    
    format = (MyMTLPixelFormat)texture.pixelFormat;
    originalFormat = (MyMTLPixelFormat)gShowSettings.originalFormat;
    
    // based on original or transcode?
    gShowSettings.isSigned = isSignedFormat(format);
    
    //gShowSettings.isSRGBShown = true;
    
    // need a way to get at KTXImage, but would need to keep mmap alive
    // this doesn't handle normals that are ASTC, so need more data from loader
    string filename = [[url.path lowercaseString] UTF8String];

    // could cycle between rrr1 and r001.
    int numChannels = numChannelsOfFormat(originalFormat);
    
    // set title to filename, chop this to just file+ext, not directory
    string filenameShort = filename;
    const char* filenameSlash = strrchr(filenameShort.c_str(), '/');
    if (filenameSlash != nullptr) {
        filenameShort = filenameSlash + 1;
    }
    
    // now chop off the extension
    filenameShort = filenameShort.substr(0, filenameShort.find_last_of("."));
    
    bool isAlbedo = false;
    bool isNormal = false;
    bool isSDF = false;
    
    // note that decoded textures are 3/4 channel even though they are normal/sdf originally, so test those first
    if (numChannels == 2 || endsWith(filenameShort, "-n") || endsWith(filenameShort, "_normal")) {
        isNormal = true;
    }
    else if (numChannels == 1 || endsWith(filenameShort, "-sdf")) {
        isSDF = true;
    }
    else if (numChannels == 3 || numChannels == 4 || endsWith(filenameShort, "-a") || endsWith(filenameShort, "_basecolor")) {
        isAlbedo = true;
    }
    
    gShowSettings.isNormal = isNormal;
    gShowSettings.isSDF = isSDF;
    
    // textures are already premul, so don't need to premul in shader
    // should really have 3 modes, unmul, default, premul
    gShowSettings.isPremul = false;
    if (isAlbedo && endsWithExtension(filename.c_str(), ".png")) {
        gShowSettings.isPremul = true; // convert to premul in shader, so can see other channels
    }
        
    if (isNormal || isSDF) {
        gShowSettings.isPremul = false;
    }
        
    gShowSettings.numChannels = numChannels;
    
    // TODO: identify if texture holds normal data from the props
    // have too many 4 channel normals that shouldn't swizzle like this
    // kramTextures.py is using etc2rg on iOS for now, and not astc.
    
    gShowSettings.isSwizzleAGToRG = false;

//    if (isASTCFormat(originalFormat) && isNormal) {
//        // channels after = "ag01"
//        gShowSettings.isSwizzleAGToRG = true;
//    }
        
    // then can manipulate this after loading
    gShowSettings.mipLOD = 0;
    gShowSettings.faceNumber = 0;
    gShowSettings.arrayNumber = 0;
    gShowSettings.sliceNumber = 0;
    
    // can derive these from texture queries
    gShowSettings.maxLOD = (int)texture.mipmapLevelCount;
    gShowSettings.faceCount = (texture.textureType == MTLTextureTypeCube ||
                               texture.textureType == MTLTextureTypeCubeArray) ? 6 : 0;
    gShowSettings.arrayCount = (int)texture.arrayLength;
    gShowSettings.sliceCount = (int)texture.depth;
    
    gShowSettings.channels = TextureChannels::ModeRGBA;
    
    gShowSettings.imageBoundsX = (int)texture.width;
    gShowSettings.imageBoundsY = (int)texture.height;
    
    [self updateViewTransforms];
    
    // this controls viewMatrix (global to all visible textures)
    gShowSettings.panX = 0.0f;
    gShowSettings.panY = 0.0f;
    
    gShowSettings.zoom = gShowSettings.zoomFit;
    
    gShowSettings.debugMode = DebugModeNone;
    
    // have one of these for each texture added to the viewer
    float scaleX = MAX(1, texture.width);
    float scaleY = MAX(1, texture.height);
    _modelMatrix = float4x4(simd_make_float4(scaleX, scaleY, 1.0f, 1.0f));
    _modelMatrix = _modelMatrix * matrix4x4_translation(0.0f, 0.0f, -1.0);

    //_modelMatrix = matrix4x4_translation(0.0f, 0.0f, 0.0f) * _modelMatrix;

    // TODO: also have Mac not be sandboxed, or figure out if drag/drop allows
    // access to folder, ktx, or png files.
    
    // TODO: Downsample images by integer multiple to fit in the current window
    // and have some sort of hierarchy to pick a given image.
    
    // TODO: what about a setting to 0 out a channel.  Can toggle only, default, off.
    // would then be able to turn on/off channels to see image without them.
    
    return YES;
}

- (float4x4)computeImageTransform:(float)panX panY:(float)panY zoom:(float)zoom {
    // translate
    float4x4 panTransform = matrix4x4_translation(-panX, panY, 0.0);
    
    // scale
    float4x4 viewMatrix = float4x4(simd_make_float4(zoom, zoom, 1.0f, 1.0f));
    viewMatrix = panTransform * viewMatrix;
    
    return _projectionMatrix * viewMatrix * _modelMatrix;
}

- (void)_updateGameState
{
    /// Update any game state before encoding renderint commands to our drawable

    Uniforms& uniforms = *(Uniforms*)_dynamicUniformBuffer[_uniformBufferIndex].contents;

    uniforms.mipLOD = gShowSettings.mipLOD;
    
    uniforms.isNormal = gShowSettings.isNormal;
    uniforms.isPremul = gShowSettings.isPremul;
    uniforms.isSigned = gShowSettings.isSigned;
    uniforms.isSwizzleAGToRG = gShowSettings.isSwizzleAGToRG;
    
    uniforms.isSDF = gShowSettings.isSDF;
    uniforms.numChannels = gShowSettings.numChannels;
    
    MyMTLTextureType textureType = MyMTLTextureType2D;
    MyMTLPixelFormat textureFormat = MyMTLPixelFormatInvalid;
    if (_colorMap) {
        textureType = (MyMTLTextureType)_colorMap.textureType;
        textureFormat = (MyMTLPixelFormat)_colorMap.pixelFormat;
    }
    
    uniforms.isCheckerboardShown = gShowSettings.isCheckerboardShown;
    bool canWrap = true;
    if (textureType == MyMTLTextureTypeCube || textureType == MyMTLTextureTypeCubeArray) {
        canWrap = false;
    }
    
    uniforms.isWrap = canWrap ? gShowSettings.isWrap : false;
    
    uniforms.isPreview = gShowSettings.isPreview;
    
    uniforms.gridX = 0;
    uniforms.gridY = 0;
    
    if (gShowSettings.isPixelGridShown) {
        uniforms.gridX = 1;
        uniforms.gridY = 1;
    }
    else if (gShowSettings.isBlockGridShown) {
        
        if (gShowSettings.blockX > 1) {
            uniforms.gridX = gShowSettings.blockX;
            uniforms.gridY = gShowSettings.blockY;
        }
    }
    
    // no debug mode when preview kicks on, make it possible to toggle back and forth more easily
    uniforms.debugMode = gShowSettings.isPreview ? DebugModeNone : gShowSettings.debugMode;
    uniforms.channels = gShowSettings.channels;

    // translate
    float4x4 panTransform = matrix4x4_translation(-gShowSettings.panX, gShowSettings.panY, 0.0);
    
    // scale
    _viewMatrix = float4x4(simd_make_float4(gShowSettings.zoom, gShowSettings.zoom, 1.0f, 1.0f));
    _viewMatrix = panTransform * _viewMatrix;
    
    // viewMatrix should typically be the inverse
    //_viewMatrix = simd_inverse(_viewMatrix);
    
    float4x4 projectionViewMatrix = _projectionMatrix * _viewMatrix;
    
    uniforms.projectionViewMatrix = projectionViewMatrix;

    // works when only one texture, but switch to projectViewMatrix
    uniforms.modelMatrix = _modelMatrix;
    
    // this was stored so view could use it, but now that code calcs the transform via computeImageTransform
    gShowSettings.projectionViewModelMatrix = projectionViewMatrix * _modelMatrix;
    
    uniforms.arrayOrSlice = 0;
    uniforms.face  = 0;
    
    // TODO: set texture specific uniforms, but using single _colorMap for now
    switch(textureType) {
        case MyMTLTextureType2D:
            // nothing
            break;
        case MyMTLTextureType3D:
            uniforms.arrayOrSlice = gShowSettings.sliceNumber;
            break;
        case MyMTLTextureTypeCube:
            uniforms.face = gShowSettings.faceNumber;
            break;
            
        case MyMTLTextureTypeCubeArray:
            uniforms.face = gShowSettings.faceNumber;
            uniforms.arrayOrSlice = gShowSettings.arrayNumber;
            break;
        case MyMTLTextureType2DArray:
            uniforms.arrayOrSlice = gShowSettings.arrayNumber;
            break;
        case MyMTLTextureType1DArray:
            uniforms.arrayOrSlice = gShowSettings.arrayNumber;
            break;
        
        default:
            break;
    }
    
    //_rotation += .01;
}


- (void)drawInMTKView:(nonnull MTKView *)view
{
    /// Per frame updates here

    // TODO: move this out, needs to get called off mouseMove, but don't want to call drawMain
    [self drawSample];
    
    dispatch_semaphore_wait(_inFlightSemaphore, DISPATCH_TIME_FOREVER);

    _uniformBufferIndex = (_uniformBufferIndex + 1) % MaxBuffersInFlight;

    id <MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    commandBuffer.label = @"MyCommand";

    __block dispatch_semaphore_t block_sema = _inFlightSemaphore;
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> /* buffer */)
     {
         dispatch_semaphore_signal(block_sema);
     }];

    [self _updateGameState];
    
    // use to autogen mipmaps if needed, might eliminate this since it's always box filter
    // TODO: do mips via kram instead, but was useful for pow-2 mip comparisons.
    
    // also use to readback pixels
    // also use for async texture upload
    bool needsBlit = _loader.isMipgenNeeded && _colorMap.mipmapLevelCount > 1;
    if (needsBlit) {
        id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
        blitEncoder.label = @"MyBlitEncoder";
        
        // autogen mips will include srgb conversions, so toggling srgb on/off isn't quite correct
        if (_loader.mipgenNeeded) {
            [blitEncoder generateMipmapsForTexture:_colorMap];
            
            _loader.mipgenNeeded = NO;
        }
    
        [blitEncoder endEncoding];
    }
    
    
    [self drawMain:commandBuffer view:view];
    
    [commandBuffer presentDrawable:view.currentDrawable];
    [commandBuffer commit];
}

- (void)drawMain:(id<MTLCommandBuffer>)commandBuffer view:(nonnull MTKView *)view {
    /// Delay getting the currentRenderPassDescriptor until absolutely needed. This avoids
    ///   holding onto the drawable and blocking the display pipeline any longer than necessary
    MTLRenderPassDescriptor* renderPassDescriptor = view.currentRenderPassDescriptor;

    if (renderPassDescriptor == nil) {
        return;
    }
    if (_colorMap == nil) {
        // this will clear target
        id<MTLRenderCommandEncoder> renderEncoder =
        [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
        renderEncoder.label = @"MainRender";
        [renderEncoder endEncoding];
        
        return;
    }
    
    /// Final pass rendering code here
    id<MTLRenderCommandEncoder> renderEncoder =
    [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
    renderEncoder.label = @"MainRender";

    // set raster state
    [renderEncoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [renderEncoder setCullMode:MTLCullModeBack];
    [renderEncoder setDepthStencilState:_depthStateFull];

    [renderEncoder pushDebugGroup:@"DrawBox"];

    // set the mesh shape
    for (NSUInteger bufferIndex = 0; bufferIndex < _mesh.vertexBuffers.count; bufferIndex++)
    {
        MTKMeshBuffer *vertexBuffer = _mesh.vertexBuffers[bufferIndex];
        if((NSNull*)vertexBuffer != [NSNull null])
        {
            [renderEncoder setVertexBuffer:vertexBuffer.buffer
                                    offset:vertexBuffer.offset
                                   atIndex:bufferIndex];
        }
    }

    //for (texture in _textures) // TODO: setup
    //if (_colorMap)
    {
        // TODO: set texture specific uniforms, but using single _colorMap for now
        bool canWrap = true;
        
        switch(_colorMap.textureType) {
            case MTLTextureType1DArray:
                [renderEncoder setRenderPipelineState:_pipelineState1DArray];
                break;
                
            case MTLTextureType2D:
                [renderEncoder setRenderPipelineState:_pipelineStateImage];
                break;
                
            case MTLTextureType2DArray:
                [renderEncoder setRenderPipelineState:_pipelineStateImageArray];
                break;
                
            case MTLTextureType3D:
                [renderEncoder setRenderPipelineState:_pipelineStateVolume];
                break;
            case MTLTextureTypeCube:
                [renderEncoder setRenderPipelineState:_pipelineStateCube];
                canWrap = false;
                
                break;
            case MTLTextureTypeCubeArray:
                canWrap = false;
                [renderEncoder setRenderPipelineState:_pipelineStateCubeArray];
                break;
                
            default:
                break;
        }
        
        id<MTLBuffer> uniformBuffer = _dynamicUniformBuffer[_uniformBufferIndex];
        [renderEncoder setVertexBuffer:uniformBuffer
                                offset:0
                               atIndex:BufferIndexUniforms];

        [renderEncoder setFragmentBuffer:uniformBuffer
                                  offset:0
                                 atIndex:BufferIndexUniforms];

        
        if (gShowSettings.isPreview) {
            // use exisiting lod, and mip
            [renderEncoder setFragmentSamplerState:(canWrap && gShowSettings.isWrap) ? _colorMapSamplerBilinearWrap : _colorMapSamplerBilinearClamp
                                  atIndex:SamplerIndexColor];
        }
        else {
            // force lod, and don't mip
            [renderEncoder setFragmentSamplerState:(canWrap && gShowSettings.isWrap) ? _colorMapSamplerWrap : _colorMapSamplerClamp
                                  lodMinClamp:gShowSettings.mipLOD
                                  lodMaxClamp:gShowSettings.mipLOD + 1
                                  atIndex:SamplerIndexColor];
        }

        // allow toggling on/off srgb, but any autogen mips on png have already done srgb reads
        id<MTLTexture> texture = _colorMap;
//        if ((!gShowSettings.isSRGBShown) && _colorMapView) {
//            texture = _colorMapView;
//        }
        
        [renderEncoder setFragmentTexture:texture
                                  atIndex:TextureIndexColor];

        for(MTKSubmesh *submesh in _mesh.submeshes)
        {
            [renderEncoder drawIndexedPrimitives:submesh.primitiveType
                                      indexCount:submesh.indexCount
                                       indexType:submesh.indexType
                                     indexBuffer:submesh.indexBuffer.buffer
                               indexBufferOffset:submesh.indexBuffer.offset];
        }
    }
    
    [renderEncoder popDebugGroup];

    
    [renderEncoder endEncoding];
    
    // TODO: run any post-processing on each texture visible as fsw
    // TODO: environment map preview should be done as fsq
}

// want to run samples independent of redrawing the main view
- (void)drawSample
{
    // Note: this is failing when running via Cmake
    bool doSample = true;
    if (!doSample) {
        return;
    }
    if (_colorMap == nil) {
        return;
    }
    
    id <MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
    commandBuffer.label = @"MyCommand";

    int textureLookupX = gShowSettings.textureLookupX;
    int textureLookupY = gShowSettings.textureLookupY;
    
    [self drawSamples:commandBuffer lookupX:textureLookupX lookupY:textureLookupY];
    
    // Synchronize the managed texture.
    id <MTLBlitCommandEncoder> blitCommandEncoder = [commandBuffer blitCommandEncoder];
    [blitCommandEncoder synchronizeResource:_sampleTex];
    [blitCommandEncoder endEncoding];

    // After synchonization, copy value back to the cpu
    id<MTLTexture> texture = _sampleTex;
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> /* buffer */)
    {
        // only 1 pixel in the texture right now
        float4 data;
        
        // copy from texture back to CPU, might be easier using MTLBuffer.contents
        MTLRegion region = {
            { 0, 0, 0 }, // MTLOrigin
            { 1, 1, 1 }  // MTLSize
        };
        
        [texture getBytes:&data bytesPerRow:16 fromRegion:region mipmapLevel:0];
        
        // return the value at the sample
        gShowSettings.textureResult = data;
        gShowSettings.textureResultX = textureLookupX;
        gShowSettings.textureResultY = textureLookupY;
        
        //printf("Color %f %f %f %f\n", data.x, data.y, data.z, data.w);
    }];
    
    [commandBuffer commit];
}


- (void)drawSamples:(id<MTLCommandBuffer>)commandBuffer lookupX:(int)lookupX lookupY:(int)lookupY {
    
    // Final pass rendering code here
    id<MTLComputeCommandEncoder> renderEncoder = [commandBuffer computeCommandEncoder];
    renderEncoder.label = @"SampleCompute";

    [renderEncoder pushDebugGroup:@"DrawBox"];

    UniformsCS uniforms;
    uniforms.uv.x = lookupX;
    uniforms.uv.y = lookupY;
    
    uniforms.face = gShowSettings.faceNumber;
    uniforms.arrayOrSlice = gShowSettings.arrayNumber;
    if (gShowSettings.sliceNumber) {
        uniforms.arrayOrSlice = gShowSettings.sliceNumber;
    }
    uniforms.mipLOD = gShowSettings.mipLOD;
    
    // run compute here, don't need a shape
    switch(_colorMap.textureType) {
        case MTLTextureType1DArray:
            [renderEncoder setComputePipelineState:_pipelineState1DArrayCS];
            break;
            
        case MTLTextureType2D:
            [renderEncoder setComputePipelineState:_pipelineStateImageCS];
            break;
            
        case MTLTextureType2DArray:
            [renderEncoder setComputePipelineState:_pipelineStateImageArrayCS];
            break;
            
        case MTLTextureType3D:
            [renderEncoder setComputePipelineState:_pipelineStateVolumeCS];
            break;
        case MTLTextureTypeCube:
            [renderEncoder setComputePipelineState:_pipelineStateCubeCS];
            break;
        case MTLTextureTypeCubeArray:
            [renderEncoder setComputePipelineState:_pipelineStateCubeArrayCS];
            break;
            
        default:
            break;
    }

    // input and output texture
    [renderEncoder setTexture:_colorMap
                              atIndex:TextureIndexColor];
    
    [renderEncoder setTexture:_sampleTex
                      atIndex:TextureIndexSamples];
    
    [renderEncoder setBytes:&uniforms length:sizeof(UniformsCS) atIndex:BufferIndexUniformsCS];
    
    // sample and copy back pixels off the offset
    [renderEncoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    
    [renderEncoder popDebugGroup];
    [renderEncoder endEncoding];
}


- (void)mtkView:(nonnull MTKView *)view drawableSizeWillChange:(CGSize)size
{
    /// Respond to drawable size or orientation changes here
    gShowSettings.viewSizeX = size.width;
    gShowSettings.viewSizeY = size.height;
    
    // TODO: only set this when size changes, but for now keep setting here and adjust zoom
    CGFloat framebufferScale = view.window.screen.backingScaleFactor ? view.window.screen.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    
    gShowSettings.viewContentScaleFactor = framebufferScale;
    
    [self updateViewTransforms];
}

- (void)updateViewTransforms {
    
    //float aspect = size.width / (float)size.height;
    //_projectionMatrix = perspective_rhs(45.0f * (M_PI / 180.0f), aspect, 0.1f, 100.0f);
    _projectionMatrix = orthographic_rhs(gShowSettings.viewSizeX, gShowSettings.viewSizeY, 0.1f, 100.0f);
    
    // DONE: adjust zoom to fit the entire image to the window
    gShowSettings.zoomFit = MIN((float)gShowSettings.viewSizeX,  (float)gShowSettings.viewSizeY) /
        MAX(1, MAX((float)gShowSettings.imageBoundsX, (float)gShowSettings.imageBoundsY));
    
    // already using drawableSize which includes scale
    // TODO: remove contentScaleFactor of view, this can be 1.0 to 2.0f
    // why does this always report 2x even when I change monitor res.
    //gShowSettings.zoomFit /= gShowSettings.viewContentScaleFactor;
}

// TODO: replace all this math

#pragma mark Matrix Math Utilities

float4x4 matrix4x4_translation(float tx, float ty, float tz)
{
    float4x4 m = {
        (float4){ 1,   0,  0,  0 },
        (float4){ 0,   1,  0,  0 },
        (float4){ 0,   0,  1,  0 },
        (float4){ tx, ty, tz,  1 }
    };
    return m;
}

//static float4x4 matrix4x4_rotation(float radians, vector_float3 axis)
//{
//    axis = vector_normalize(axis);
//    float ct = cosf(radians);
//    float st = sinf(radians);
//    float ci = 1 - ct;
//    float x = axis.x, y = axis.y, z = axis.z;
//
//    float4x4 m = {
//        (float4){ ct + x * x * ci,     y * x * ci + z * st, z * x * ci - y * st, 0},
//        (float4){ x * y * ci - z * st,     ct + y * y * ci, z * y * ci + x * st, 0},
//        (float4){ x * z * ci + y * st, y * z * ci - x * st,     ct + z * z * ci, 0},
//        (float4){                   0,                   0,                   0, 1}
//    };
//    return m;
//}
//
//float4x4 perspective_rhs(float fovyRadians, float aspect, float nearZ, float farZ)
//{
//    float ys = 1 / tanf(fovyRadians * 0.5);
//    float xs = ys / aspect;
//    float zs = farZ / (nearZ - farZ);
//
//    TODO: handle isReverseZ if add option to draw with perspective
//    float4x4 m = {
//        (float4){ xs,   0,          0,  0 },
//        (float4){  0,  ys,          0,  0 },
//        (float4){  0,   0,         zs, -1 },
//        (float4){  0,   0, nearZ * zs,  0 }
//    };
//    return m;
//}

float4x4 orthographic_rhs(float width, float height, float nearZ, float farZ)
{
    //float aspectRatio = width / height;
    float xs = 2.0f/width;
    float ys = 2.0f/height;
    
    float xoff = 0.0f; // -0.5f * width;
    float yoff = 0.0f; // -0.5f * height;
    
    float dz = -(farZ - nearZ);
    float zs = 1.0f / dz;
    
    float m22 = zs;
    float m23 = zs * nearZ;

    // revZ, can't use infiniteZ with ortho view
    if (gShowSettings.isReverseZ) {
        m22 = -m22;
        m23 = 1.0f - m23;
    }
    
    float4x4 m = {
        (float4){ xs,   0,      0,  0 },
        (float4){  0,   ys,     0,  0 },
        (float4){ 0,     0,     m22, 0 },
        (float4){ xoff, yoff,    m23,  1 }
    };
    return m;
}


@end
