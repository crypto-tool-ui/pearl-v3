#ifndef CP_E2E_H
#define CP_E2E_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int cp_e2e_start(int mode, uint8_t* out, size_t cap, size_t* out_len,
                 void** state, uint8_t* err, size_t err_cap);
int cp_e2e_finish(void* state, const uint8_t* hello, size_t hello_len,
                  void** session, uint8_t* err, size_t err_cap);
int cp_e2e_encrypt(void* session, const uint8_t* input, size_t input_len,
                   uint8_t* out, size_t cap, size_t* out_len,
                   uint8_t* err, size_t err_cap);
int cp_e2e_decrypt(void* session, const uint8_t* input, size_t input_len,
                   uint8_t* out, size_t cap, size_t* out_len,
                   uint8_t* err, size_t err_cap);
void cp_e2e_destroy(void* session);
void cp_e2e_abort_handshake(void* state);

#ifdef __cplusplus
}
#endif

#endif
