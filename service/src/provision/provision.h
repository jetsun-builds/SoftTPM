#ifndef SOFTTPM_PROVISION_H
#define SOFTTPM_PROVISION_H

int provision_run(const wchar_t* StateDirectory);

int provision_revoke_attestation(void);

#endif
