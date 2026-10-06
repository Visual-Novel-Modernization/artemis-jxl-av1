// jxl2png.cpp - JXL bytes in, PNG bytes out. The engine's own libpng path does
// the rest, so colour, palette, alpha and row layout stay identical.
#include "jxl2png.h"

#include <jxl/decode.h>
#include <png.h>

#include <setjmp.h>
#include <string.h>
#include <vector>

namespace {

struct PngSink {
    std::vector<unsigned char> *buf;
};

void PngWriteCb(png_structp png, png_bytep data, png_size_t len) {
    PngSink *s = (PngSink *)png_get_io_ptr(png);
    s->buf->insert(s->buf->end(), data, data + len);
}

void PngFlushCb(png_structp) {}

}

bool JxlIsJxl(const unsigned char *data, size_t size) {
    if (!data || size < 2) return false;
    JxlSignature sig = JxlSignatureCheck(data, size);
    return sig == JXL_SIG_CODESTREAM || sig == JXL_SIG_CONTAINER;
}

bool JxlToPng(const unsigned char *in, size_t inSize,
              std::vector<unsigned char> &outPng, int *outW, int *outH,
              int *outChannels, char *errBuf, size_t errBufSize) {
    outPng.clear();
    if (errBuf && errBufSize) errBuf[0] = 0;

    JxlDecoder *dec = JxlDecoderCreate(NULL);
    if (!dec) {
        if (errBuf) strncpy(errBuf, "JxlDecoderCreate failed", errBufSize - 1);
        return false;
    }

    JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
    JxlDecoderSetInput(dec, in, inSize);
    JxlDecoderCloseInput(dec);

    JxlBasicInfo info;
    memset(&info, 0, sizeof(info));
    bool haveInfo = false;
    std::vector<unsigned char> pixels;
    unsigned channels = 0;

    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        if (st == JXL_DEC_ERROR || st == JXL_DEC_NEED_MORE_INPUT) {
            if (errBuf) sprintf(errBuf, "JXL decode error (status=%d)", (int)st);
            JxlDecoderDestroy(dec);
            return false;
        }
        if (st == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) {
                if (errBuf) strncpy(errBuf, "GetBasicInfo failed", errBufSize - 1);
                JxlDecoderDestroy(dec);
                return false;
            }
            haveInfo = true;

            bool hasAlpha = info.alpha_bits > 0;
            if (hasAlpha)
                channels = 4;
            else if (info.num_color_channels == 1)
                channels = 1;
            else
                channels = 3;

            JxlPixelFormat fmt;
            fmt.num_channels = channels;
            fmt.data_type = JXL_TYPE_UINT8;
            fmt.endianness = JXL_NATIVE_ENDIAN;
            fmt.align = 0;

            size_t sz = 0;
            if (JxlDecoderImageOutBufferSize(dec, &fmt, &sz) != JXL_DEC_SUCCESS) {
                if (errBuf) strncpy(errBuf, "ImageOutBufferSize failed", errBufSize - 1);
                JxlDecoderDestroy(dec);
                return false;
            }
            pixels.resize(sz);
            if (JxlDecoderSetImageOutBuffer(dec, &fmt, pixels.data(), sz) != JXL_DEC_SUCCESS) {
                if (errBuf) strncpy(errBuf, "SetImageOutBuffer failed", errBufSize - 1);
                JxlDecoderDestroy(dec);
                return false;
            }
        } else if (st == JXL_DEC_FULL_IMAGE) {
            break;
        } else if (st == JXL_DEC_SUCCESS) {
            break;
        }
    }
    JxlDecoderDestroy(dec);

    if (!haveInfo || pixels.empty()) {
        if (errBuf) strncpy(errBuf, "no pixel data produced", errBufSize - 1);
        return false;
    }
    if (info.xsize == 0 || info.ysize == 0) {
        if (errBuf) strncpy(errBuf, "zero dimension", errBufSize - 1);
        return false;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        if (errBuf) strncpy(errBuf, "png_create_write_struct failed", errBufSize - 1);
        return false;
    }
    png_infop ip = png_create_info_struct(png);
    if (!ip) {
        png_destroy_write_struct(&png, NULL);
        if (errBuf) strncpy(errBuf, "png_create_info_struct failed", errBufSize - 1);
        return false;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &ip);
        if (errBuf) strncpy(errBuf, "libpng longjmp during encode", errBufSize - 1);
        return false;
    }

    PngSink sink;
    sink.buf = &outPng;
    png_set_write_fn(png, &sink, PngWriteCb, PngFlushCb);
    // level 1 and no filtering: this staging buffer never reaches disk
    png_set_compression_level(png, 1);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);

    int colorType = (channels == 4)   ? PNG_COLOR_TYPE_RGBA
                    : (channels == 3) ? PNG_COLOR_TYPE_RGB
                                      : PNG_COLOR_TYPE_GRAY;
    png_set_IHDR(png, ip, info.xsize, info.ysize, 8, colorType,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, ip);

    size_t stride = (size_t)info.xsize * channels;
    std::vector<png_bytep> rows(info.ysize);
    for (unsigned y = 0; y < info.ysize; ++y)
        rows[y] = (png_bytep)(pixels.data() + (size_t)y * stride);

    png_write_image(png, rows.data());
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &ip);

    if (outW) *outW = (int)info.xsize;
    if (outH) *outH = (int)info.ysize;
    if (outChannels) *outChannels = (int)channels;
    return true;
}
