#pragma once

#include <stdbool.h>
#include <stdint.h>

void voice_prov_negotiate(uint8_t *data, int len, uint8_t **output_data,
                          int *output_len, bool *need_free);
int voice_prov_encrypt(uint8_t iv8, uint8_t *data, int len);
int voice_prov_decrypt(uint8_t iv8, uint8_t *data, int len);
uint16_t voice_prov_checksum(uint8_t iv8, uint8_t *data, int len);
int voice_prov_security_init(void);
void voice_prov_security_deinit(void);
