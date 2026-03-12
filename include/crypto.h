#ifndef VIPER_CRYPTO_H
#define VIPER_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

void viper_sha256(const uint8_t* data, size_t len, uint8_t hash[32]);
void viper_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* data, size_t data_len, uint8_t mac[32]);
int viper_base64_encode(const uint8_t* src, size_t len, char* dst, size_t dst_len);
int viper_base64_decode(const char* src, uint8_t* dst, size_t dst_len);

#endif // VIPER_CRYPTO_H
