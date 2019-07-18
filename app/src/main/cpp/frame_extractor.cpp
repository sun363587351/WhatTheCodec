//
// Created by Alex Javernaut on 2019-07-12.
//

#include "frame_extractor.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <android/bitmap.h>
#include "video_config.h"
#include "log.h"

bool frame_extractor_fill_with_preview(JNIEnv *env, jobject jVideoConfig, jobjectArray jBitmaps) {
    int arraySize = env->GetArrayLength(jBitmaps);

    AndroidBitmapInfo bitmapMetricInfo;
    AndroidBitmap_getInfo(env, env->GetObjectArrayElement(jBitmaps, 0), &bitmapMetricInfo);

    auto *videoConfig = video_config_get(jVideoConfig);

    auto pixelFormat = static_cast<AVPixelFormat>(videoConfig->parameters->format);
    if (pixelFormat == AV_PIX_FMT_NONE) {
        // With pipe protocol some files fail to provide pixel format info.
        // In this case we can't establish neither scaling nor simple frame extracting.
        return false;
    }

    bool resultValue = true;

    SwsContext *scalingContext =
            sws_getContext(
                    // srcW
                    videoConfig->parameters->width,
                    // srcH
                    videoConfig->parameters->height,
                    // srcFormat
                    pixelFormat,
                    // dstW
                    bitmapMetricInfo.width,
                    // dstH
                    bitmapMetricInfo.height,
                    // dstFormat
                    AV_PIX_FMT_RGBA,
                    SWS_BICUBIC, nullptr, nullptr, nullptr);

    int64_t videoDuration = videoConfig->
            avFormatContext->
            streams[videoConfig->videoStreamIndex]->
            duration;

    for (int pos = 0; pos < arraySize; pos++) {
        jobject jBitmap = env->GetObjectArrayElement(jBitmaps, pos);

        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();

        int64_t seekPosition = videoDuration / arraySize * pos;
        if (videoConfig->fullFeatured || seekPosition > 0) {
            av_seek_frame(videoConfig->avFormatContext,
                          videoConfig->videoStreamIndex,
                          seekPosition,
                          0);
        }

        AVCodecContext *videoCodecContext = avcodec_alloc_context3(videoConfig->avVideoCodec);
        avcodec_parameters_to_context(videoCodecContext, videoConfig->parameters);
        avcodec_open2(videoCodecContext, videoConfig->avVideoCodec, nullptr);

        while (true) {
            if (av_read_frame(videoConfig->avFormatContext, packet) < 0) {
                resultValue = false;
                break;
            }

            if (packet->stream_index == videoConfig->videoStreamIndex) {
                avcodec_send_packet(videoCodecContext, packet);
                int response = avcodec_receive_frame(videoCodecContext, frame);
                if (response == AVERROR(EAGAIN)) {
                    continue;
                }

                if (response >= 0) {
                    AVFrame *frameForDrawing = av_frame_alloc();
                    void *bitmapBuffer;
                    AndroidBitmap_lockPixels(env, jBitmap, &bitmapBuffer);

                    av_image_fill_arrays(
                            frameForDrawing->data,
                            frameForDrawing->linesize,
                            static_cast<const uint8_t *>(bitmapBuffer),
                            AV_PIX_FMT_RGBA,
                            bitmapMetricInfo.width,
                            bitmapMetricInfo.height,
                            1);

                    sws_scale(
                            scalingContext,
                            frame->data,
                            frame->linesize,
                            0,
                            videoConfig->parameters->height,
                            frameForDrawing->data,
                            frameForDrawing->linesize);

                    av_frame_free(&frameForDrawing);

                    AndroidBitmap_unlockPixels(env, jBitmap);
                    break;
                }
            }
            av_packet_unref(packet);
        }

        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&videoCodecContext);
    }

    sws_freeContext(scalingContext);

    return resultValue;
}