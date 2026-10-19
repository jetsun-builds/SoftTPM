#pragma warning(disable : 4996)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/buffer.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include "ek_certificate.h"
#include "log.h"
#include "storage/state_store.h"

#define SOFTTPM_CA_FILE_NAME L"ek-ca.pem"
#define SOFTTPM_CA_VALIDITY_SECONDS (10 * 365 * 24 * 60 * 60)
#define SOFTTPM_CA_COMMON_NAME "SoftTPM Local CA"
#define SOFTTPM_EK_COMMON_NAME "SoftTPM Endorsement Key"
#define SOFTTPM_ORGANIZATION "SoftTPM Project"
#define SOFTTPM_RSA_EXPONENT 65537

static void SoftTpmLogSslError(const char* Context)
{
    unsigned long error = ERR_get_error();
    char message[256];

    if (error == 0)
    {
        log_error("ek-cert: %s failed", Context);
        return;
    }

    ERR_error_string_n(error, message, sizeof(message));
    log_error("ek-cert: %s failed: %s", Context, message);
}

static void SoftTpmCaFilePath(const wchar_t* StateDirectory, wchar_t* Path, size_t PathCount)
{
    _snwprintf(Path, PathCount, L"%ls\\%ls", StateDirectory, SOFTTPM_CA_FILE_NAME);
    Path[PathCount - 1] = L'\0';
}

static int SoftTpmSetRandomSerial(X509* Certificate)
{
    unsigned char bytes[4];
    uint32_t value;
    long serial;

    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
    {
        return -1;
    }

    value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
            | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];

    serial = (long)(value & 0x7FFFFFFFu);
    if (serial == 0)
    {
        serial = 1;
    }

    if (ASN1_INTEGER_set(X509_get_serialNumber(Certificate), serial) != 1)
    {
        return -1;
    }

    return 0;
}

static int SoftTpmAddExtension(X509* Certificate, X509* Issuer, int Nid, const char* Value)
{
    X509V3_CTX context;
    X509_EXTENSION* extension;

    X509V3_set_ctx(&context, Issuer, Certificate, NULL, NULL, 0);
    extension = X509V3_EXT_conf_nid(NULL, &context, Nid, Value);

    if (extension == NULL)
    {
        return -1;
    }

    if (X509_add_ext(Certificate, extension, -1) != 1)
    {
        X509_EXTENSION_free(extension);
        return -1;
    }

    X509_EXTENSION_free(extension);
    return 0;
}

static int SoftTpmSetValidity(X509* Certificate)
{
    if (X509_gmtime_adj(X509_getm_notBefore(Certificate), 0) == NULL)
    {
        return -1;
    }

    if (X509_gmtime_adj(X509_getm_notAfter(Certificate), SOFTTPM_CA_VALIDITY_SECONDS)
        == NULL)
    {
        return -1;
    }

    return 0;
}

static int SoftTpmWriteCaFile(
    const wchar_t* Path,
    X509* Certificate,
    EVP_PKEY* Key)
{
    BIO* bio;
    BUF_MEM* memory = NULL;
    FILE* file;
    int result = -1;

    log_info("ek-cert: building the CA PEM data");

    bio = BIO_new(BIO_s_mem());

    if (bio == NULL)
    {
        log_error("ek-cert: cannot allocate the CA PEM buffer");
        return -1;
    }

    if (PEM_write_bio_X509(bio, Certificate) != 1)
    {
        log_error("ek-cert: cannot encode the CA certificate");
        goto cleanup;
    }

    if (PEM_write_bio_PrivateKey(bio, Key, NULL, NULL, 0, NULL, NULL) != 1)
    {
        log_error("ek-cert: cannot encode the CA private key");
        goto cleanup;
    }

    BIO_get_mem_ptr(bio, &memory);

    if (memory == NULL || memory->length == 0)
    {
        log_error("ek-cert: CA PEM data is empty");
        goto cleanup;
    }

    log_info("ek-cert: writing %u CA PEM bytes", (unsigned)memory->length);

    file = _wfopen(Path, L"wb");

    if (file == NULL)
    {
        log_error("ek-cert: cannot open CA file for writing");
        goto cleanup;
    }

    if (fwrite(memory->data, 1, memory->length, file) != memory->length)
    {
        log_error("ek-cert: cannot write the CA file");
        fclose(file);
        goto cleanup;
    }

    fflush(file);
    fclose(file);

    log_info("ek-cert: CA file written");
    result = 0;

cleanup:
    BIO_free(bio);
    return result;
}

static int SoftTpmReadCaFile(
    const wchar_t* Path,
    X509** Certificate,
    EVP_PKEY** Key)
{
    FILE* file = _wfopen(Path, L"rb");
    unsigned char* buffer = NULL;
    unsigned char* grown;
    size_t size = 0;
    size_t used = 0;
    size_t got;
    BIO* bio;

    if (file == NULL)
    {
        log_info("ek-cert: CA file not present (%ls)", Path);
        return -1;
    }

    for (;;)
    {
        if (used == size)
        {
            size_t next = (size == 0) ? 4096 : size * 2;

            grown = (unsigned char*)realloc(buffer, next);

            if (grown == NULL)
            {
                free(buffer);
                fclose(file);
                return -1;
            }

            buffer = grown;
            size = next;
        }

        got = fread(buffer + used, 1, size - used, file);
        used += got;

        if (got == 0)
        {
            break;
        }
    }

    fclose(file);

    log_info("ek-cert: CA file read (%u bytes)", (unsigned)used);

    if (used == 0)
    {
        log_info("ek-cert: CA file is empty");
        free(buffer);
        return -1;
    }

    bio = BIO_new_mem_buf(buffer, (int)used);

    if (bio == NULL)
    {
        free(buffer);
        return -1;
    }

    *Certificate = PEM_read_bio_X509(bio, NULL, NULL, NULL);

    if (*Certificate != NULL)
    {
        *Key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    }

    log_info(
        "ek-cert: CA certificate %s",
        (*Certificate != NULL) ? "loaded" : "not readable");

    BIO_free(bio);
    free(buffer);

    if (*Certificate == NULL || *Key == NULL)
    {
        return -1;
    }

    return 0;
}

static int SoftTpmCreateCa(
    const wchar_t* Path,
    X509** Certificate,
    EVP_PKEY** Key,
    int* Created)
{
    RSA* rsa = NULL;
    BIGNUM* exponent = NULL;
    EVP_PKEY* key = NULL;
    X509* certificate = NULL;
    X509_NAME* name;

    *Created = 0;

    rsa = RSA_new();
    exponent = BN_new();
    key = EVP_PKEY_new();
    certificate = X509_new();

    if (rsa == NULL || exponent == NULL || key == NULL || certificate == NULL)
    {
        goto cleanup;
    }

    if (BN_set_word(exponent, RSA_F4) != 1)
    {
        goto cleanup;
    }

    if (RSA_generate_key_ex(rsa, 2048, exponent, NULL) != 1)
    {
        goto cleanup;
    }

    log_info("ek-cert: CA key generated");

    if (EVP_PKEY_assign_RSA(key, rsa) != 1)
    {
        goto cleanup;
    }

    rsa = NULL;

    if (X509_set_version(certificate, 2) != 1)
    {
        goto cleanup;
    }

    if (SoftTpmSetRandomSerial(certificate) != 0)
    {
        goto cleanup;
    }

    if (SoftTpmSetValidity(certificate) != 0)
    {
        goto cleanup;
    }

    if (X509_set_pubkey(certificate, key) != 1)
    {
        goto cleanup;
    }

    name = X509_get_subject_name(certificate);

    if (name == NULL
        || X509_NAME_add_entry_by_txt(
               name, "CN", MBSTRING_ASC,
               (const unsigned char*)SOFTTPM_CA_COMMON_NAME, -1, -1, 0) != 1
        || X509_NAME_add_entry_by_txt(
               name, "O", MBSTRING_ASC,
               (const unsigned char*)SOFTTPM_ORGANIZATION, -1, -1, 0) != 1)
    {
        goto cleanup;
    }

    if (X509_set_issuer_name(certificate, name) != 1)
    {
        goto cleanup;
    }

    if (SoftTpmAddExtension(
            certificate, certificate, NID_basic_constraints, "critical,CA:TRUE")
        != 0)
    {
        goto cleanup;
    }

    if (SoftTpmAddExtension(
            certificate, certificate, NID_key_usage, "critical,keyCertSign,cRLSign")
        != 0)
    {
        goto cleanup;
    }

    if (X509_sign(certificate, key, EVP_sha256()) == 0)
    {
        goto cleanup;
    }

    log_info("ek-cert: CA certificate signed");

    if (SoftTpmWriteCaFile(Path, certificate, key) != 0)
    {
        goto cleanup;
    }

    *Certificate = certificate;
    *Key = key;
    *Created = 1;
    return 0;

cleanup:
    if (rsa != NULL)
    {
        RSA_free(rsa);
    }
    if (exponent != NULL)
    {
        BN_free(exponent);
    }
    if (key != NULL)
    {
        EVP_PKEY_free(key);
    }
    if (certificate != NULL)
    {
        X509_free(certificate);
    }

    return -1;
}

int ek_certificate_ensure_ca(const wchar_t* StateDirectory)
{
    wchar_t path[SOFTTPM_STATE_MAX_PATH];
    X509* certificate = NULL;
    EVP_PKEY* key = NULL;
    int created = 0;
    int result;

    SoftTpmCaFilePath(StateDirectory, path, SOFTTPM_STATE_MAX_PATH);

    if (SoftTpmReadCaFile(path, &certificate, &key) == 0)
    {
        X509_free(certificate);
        EVP_PKEY_free(key);
        return 0;
    }

    log_info("ek-cert: generating a new CA key");

    result = SoftTpmCreateCa(path, &certificate, &key, &created);

    if (result != 0)
    {
        SoftTpmLogSslError("CA key generation");
        return -1;
    }

    log_info("ek-cert: created local EK CA at %ls", path);

    X509_free(certificate);
    EVP_PKEY_free(key);
    return 0;
}

int ek_certificate_issue(
    const wchar_t* StateDirectory,
    const unsigned char* Modulus,
    size_t ModulusSize,
    unsigned char** CertificateDer,
    uint32_t* CertificateDerSize)
{
    wchar_t path[SOFTTPM_STATE_MAX_PATH];
    X509* caCertificate = NULL;
    EVP_PKEY* caKey = NULL;
    X509* certificate = NULL;
    EVP_PKEY* ekKey = NULL;
    RSA* rsa = NULL;
    BIGNUM* modulus = NULL;
    BIGNUM* exponent = NULL;
    X509_NAME* name;
    unsigned char* der = NULL;
    unsigned char* cursor;
    int derSize;
    int result = -1;

    *CertificateDer = NULL;
    *CertificateDerSize = 0;

    if (Modulus == NULL || ModulusSize == 0)
    {
        return -1;
    }

    SoftTpmCaFilePath(StateDirectory, path, SOFTTPM_STATE_MAX_PATH);

    if (SoftTpmReadCaFile(path, &caCertificate, &caKey) != 0)
    {
        if (ek_certificate_ensure_ca(StateDirectory) != 0
            || SoftTpmReadCaFile(path, &caCertificate, &caKey) != 0)
        {
            SoftTpmLogSslError("CA load");
            return -1;
        }
    }

    log_info("ek-cert: CA ready, building the EK certificate");

    certificate = X509_new();
    ekKey = EVP_PKEY_new();
    rsa = RSA_new();
    modulus = BN_bin2bn(Modulus, (int)ModulusSize, NULL);
    exponent = BN_new();

    if (certificate == NULL || ekKey == NULL || rsa == NULL
        || modulus == NULL || exponent == NULL)
    {
        goto cleanup;
    }

    if (BN_set_word(exponent, SOFTTPM_RSA_EXPONENT) != 1)
    {
        goto cleanup;
    }

    if (RSA_set0_key(rsa, modulus, exponent, NULL) != 1)
    {
        goto cleanup;
    }

    modulus = NULL;
    exponent = NULL;

    if (EVP_PKEY_assign_RSA(ekKey, rsa) != 1)
    {
        goto cleanup;
    }

    rsa = NULL;

    if (X509_set_version(certificate, 2) != 1
        || SoftTpmSetRandomSerial(certificate) != 0
        || SoftTpmSetValidity(certificate) != 0
        || X509_set_pubkey(certificate, ekKey) != 1
        || X509_set_issuer_name(certificate, X509_get_subject_name(caCertificate)) != 1)
    {
        goto cleanup;
    }

    name = X509_get_subject_name(certificate);

    if (name == NULL
        || X509_NAME_add_entry_by_txt(
               name, "CN", MBSTRING_ASC,
               (const unsigned char*)SOFTTPM_EK_COMMON_NAME, -1, -1, 0) != 1
        || X509_NAME_add_entry_by_txt(
               name, "O", MBSTRING_ASC,
               (const unsigned char*)SOFTTPM_ORGANIZATION, -1, -1, 0) != 1)
    {
        goto cleanup;
    }

    if (SoftTpmAddExtension(
            certificate, caCertificate, NID_basic_constraints, "critical,CA:FALSE")
        != 0)
    {
        goto cleanup;
    }

    if (SoftTpmAddExtension(
            certificate, caCertificate, NID_key_usage, "critical,keyEncipherment")
        != 0)
    {
        goto cleanup;
    }

    if (X509_sign(certificate, caKey, EVP_sha256()) == 0)
    {
        goto cleanup;
    }

    log_info("ek-cert: EK certificate signed");

    derSize = i2d_X509(certificate, NULL);

    if (derSize <= 0)
    {
        goto cleanup;
    }

    log_info("ek-cert: EK certificate encoded (%d bytes)", derSize);

    der = (unsigned char*)OPENSSL_malloc((size_t)derSize);

    if (der == NULL)
    {
        goto cleanup;
    }

    cursor = der;

    if (i2d_X509(certificate, &cursor) != derSize)
    {
        goto cleanup;
    }

    *CertificateDer = der;
    *CertificateDerSize = (uint32_t)derSize;
    der = NULL;
    result = 0;

cleanup:
    if (result != 0)
    {
        SoftTpmLogSslError("EK certificate issuance");
    }

    if (der != NULL)
    {
        OPENSSL_free(der);
    }
    if (modulus != NULL)
    {
        BN_free(modulus);
    }
    if (exponent != NULL)
    {
        BN_free(exponent);
    }
    if (rsa != NULL)
    {
        RSA_free(rsa);
    }
    if (ekKey != NULL)
    {
        EVP_PKEY_free(ekKey);
    }
    if (certificate != NULL)
    {
        X509_free(certificate);
    }
    if (caCertificate != NULL)
    {
        X509_free(caCertificate);
    }
    if (caKey != NULL)
    {
        EVP_PKEY_free(caKey);
    }

    return result;
}

void ek_certificate_free(unsigned char* CertificateDer)
{
    if (CertificateDer != NULL)
    {
        OPENSSL_free(CertificateDer);
    }
}
