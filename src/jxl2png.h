#pragma once
#include <stddef.h>
#include <vector>

bool JxlIsJxl(const unsigned char *data, size_t size);

// outPng holds a complete PNG file on success
bool JxlToPng(const unsigned char *in, size_t inSize,
              std::vector<unsigned char> &outPng, int *outW, int *outH,
              int *outChannels, char *errBuf, size_t errBufSize);
