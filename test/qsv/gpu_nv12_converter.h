// Copyright (c) 2026 Jonathan Eid. MIT License; see LICENSE in this directory.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <cstdio>

inline void gpu_check(HRESULT hr,const char* operation){if(FAILED(hr)){char msg[320];snprintf(msg,sizeof(msg),"%s failed: HRESULT 0x%08lx",operation,static_cast<unsigned long>(hr));throw std::runtime_error(msg);}}
class GpuNv12Converter {
 Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device;
 Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context;
 Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
 Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
 Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12;
 Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
 UINT frame{};
public:
 GpuNv12Converter(ID3D11Device* device,UINT width,UINT height,DXGI_FORMAT input_format,UINT fps_num,UINT fps_den){
  gpu_check(device->QueryInterface(IID_PPV_ARGS(&video_device)),"Get D3D11 video device");
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);gpu_check(context.As(&video_context),"Get D3D11 video context");
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};desc.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;desc.InputWidth=desc.OutputWidth=width;desc.InputHeight=desc.OutputHeight=height;desc.InputFrameRate={fps_num,fps_den};desc.OutputFrameRate=desc.InputFrameRate;desc.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  gpu_check(video_device->CreateVideoProcessorEnumerator(&desc,&enumerator),"Create video processor enumerator");
  UINT flags{};gpu_check(enumerator->CheckVideoProcessorFormat(input_format,&flags),"Check RGBA/BGRA video format");if(!(flags&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))throw std::runtime_error("Video processor cannot read input format");
  gpu_check(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12,&flags),"Check NV12 video format");if(!(flags&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))throw std::runtime_error("Video processor cannot output NV12");
  gpu_check(video_device->CreateVideoProcessor(enumerator.Get(),0,&processor),"Create GPU video processor");
  D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_NV12;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  gpu_check(device->CreateTexture2D(&td,nullptr,&nv12),"Create converted NV12 texture");
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};ov.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;gpu_check(video_device->CreateVideoProcessorOutputView(nv12.Get(),enumerator.Get(),&ov,&output_view),"Create NV12 processor output view");
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};rgb.Usage=1;rgb.YCbCr_Matrix=1;rgb.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};yuv.Usage=1;yuv.YCbCr_Matrix=1;yuv.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
  video_context->VideoProcessorSetStreamColorSpace(processor.Get(),0,&rgb);video_context->VideoProcessorSetOutputColorSpace(processor.Get(),&yuv);
  video_context->VideoProcessorSetStreamFrameFormat(processor.Get(),0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);video_context->VideoProcessorSetStreamAutoProcessingMode(processor.Get(),0,FALSE);
  RECT rect{0,0,LONG(width),LONG(height)};video_context->VideoProcessorSetStreamSourceRect(processor.Get(),0,TRUE,&rect);video_context->VideoProcessorSetStreamDestRect(processor.Get(),0,TRUE,&rect);video_context->VideoProcessorSetOutputTargetRect(processor.Get(),TRUE,&rect);
 }
 ID3D11Texture2D* convert(ID3D11Texture2D* input,Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>& input_view){
  if(!input_view){D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};iv.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;gpu_check(video_device->CreateVideoProcessorInputView(input,enumerator.Get(),&iv,&input_view),"Create RGBA/BGRA processor input view");}
  D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.OutputIndex=0;stream.InputFrameOrField=frame;stream.pInputSurface=input_view.Get();gpu_check(video_context->VideoProcessorBlt(processor.Get(),output_view.Get(),frame++,1,&stream),"GPU RGBA/BGRA to NV12 conversion");return nv12.Get();
 }
};
