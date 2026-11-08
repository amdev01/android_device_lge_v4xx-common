#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hardware/hardware.h>
#include <hardware/consumerir.h>

#ifndef CONSUMERIR_TRANSMITTER
#define CONSUMERIR_TRANSMITTER "transmitter"
#endif

int main(void)
{
    const struct hw_module_t *mod = NULL;
    struct hw_device_t *hwdev = NULL;
    consumerir_device_t *dev = NULL;
    int freq = 38000;
    int pattern[] = {
        9000, 4500,
        560, 560, 560, 1690, 560, 560, 560, 560,
        560, 1690, 560, 1690, 560, 560, 560, 560,
        560, 560, 560, 1690, 560, 560, 560, 560,
        560, 1690, 560, 560, 560, 1690, 560, 1690,
        560, 560, 560, 1690, 560, 1690, 560, 1690,
        560, 1690, 560, 560, 560, 1690, 560, 560,
        560, 1690, 560, 560, 560, 560, 560, 1690,
        560, 560, 560, 560, 560, 560, 560, 1690,
        560
    };
    int n = (int)(sizeof(pattern) / sizeof(pattern[0]));
    int rc;

    rc = hw_get_module(CONSUMERIR_HARDWARE_MODULE_ID, &mod);
    if (rc) {
        fprintf(stderr, "hw_get_module failed %d\n", rc);
        return 1;
    }
    rc = mod->methods->open(mod, CONSUMERIR_TRANSMITTER, &hwdev);
    if (rc || !hwdev) {
        fprintf(stderr, "open failed %d\n", rc);
        return 2;
    }
    dev = (consumerir_device_t *)hwdev;
    printf("transmitting %d entries @ %d Hz\n", n, freq);
    rc = dev->transmit(dev, freq, pattern, n);
    printf("transmit rc=%d\n", rc);
    if (dev->common.close)
        dev->common.close(&dev->common);
    return rc == 0 ? 0 : 3;
}
