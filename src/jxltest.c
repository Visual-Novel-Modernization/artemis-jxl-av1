#include <jxl/decode.h>
#include <stdio.h>

int main(void) {
    printf("JxlDecoderVersion = 0x%08X\n", (unsigned)JxlDecoderVersion());
    JxlDecoder *dec = JxlDecoderCreate(NULL);
    printf("JxlDecoderCreate   = %p\n", (void *)dec);
    if (dec) JxlDecoderDestroy(dec);
    return 0;
}
