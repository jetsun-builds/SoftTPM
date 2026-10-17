#ifndef SOFTTPM_EK_CERTIFICATE_H
#define SOFTTPM_EK_CERTIFICATE_H

#include <stddef.h>
#include <stdint.h>

int ek_certificate_ensure_ca(const wchar_t* StateDirectory);

int ek_certificate_issue(
    const wchar_t* StateDirectory,
    const unsigned char* Modulus,
    size_t ModulusSize,
    unsigned char** CertificateDer,
    uint32_t* CertificateDerSize);

void ek_certificate_free(unsigned char* CertificateDer);

#endif
