/*
 * Copyright 2006 Juan Lang
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */
#include <stdarg.h>
#define NONAMELESSUNION
#include "windef.h"
#include "winbase.h"
#define CERT_CHAIN_PARA_HAS_EXTRA_FIELDS
#define CERT_REVOCATION_PARA_HAS_EXTRA_FIELDS
#include "wincrypt.h"
#include "wine/debug.h"
#include "wine/unicode.h"
#include "crypt32_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypt);
WINE_DECLARE_DEBUG_CHANNEL(chain);

#define DEFAULT_CYCLE_MODULUS 7

static HCERTCHAINENGINE CRYPT_defaultChainEngine;

/* This represents a subset of a certificate chain engine:  it doesn't include
 * the "hOther" store described by MSDN, because I'm not sure how that's used.
 * It also doesn't include the "hTrust" store, because I don't yet implement
 * CTLs or complex certificate chains.
 */
typedef struct _CertificateChainEngine
{
    LONG       ref;
    HCERTSTORE hRoot;
    HCERTSTORE hWorld;
    DWORD      dwFlags;
    DWORD      dwUrlRetrievalTimeout;
    DWORD      MaximumCachedCertificates;
    DWORD      CycleDetectionModulus;
} CertificateChainEngine, *PCertificateChainEngine;

static inline void CRYPT_AddStoresToCollection(HCERTSTORE collection,
 DWORD cStores, HCERTSTORE *stores)
{
    DWORD i;

    for (i = 0; i < cStores; i++)
        CertAddStoreToCollection(collection, stores[i], 0, 0);
}

static inline void CRYPT_CloseStores(DWORD cStores, HCERTSTORE *stores)
{
    DWORD i;

    for (i = 0; i < cStores; i++)
        CertCloseStore(stores[i], 0);
}

static const WCHAR rootW[] = { 'R','o','o','t',0 };

/* Finds cert in store by comparing the cert's hashes. */
static PCCERT_CONTEXT CRYPT_FindCertInStore(HCERTSTORE store,
 PCCERT_CONTEXT cert)
{
    PCCERT_CONTEXT matching = NULL;
    BYTE hash[20];
    DWORD size = sizeof(hash);

    if (CertGetCertificateContextProperty(cert, CERT_HASH_PROP_ID, hash, &size))
    {
        CRYPT_HASH_BLOB blob = { sizeof(hash), hash };

        matching = CertFindCertificateInStore(store, cert->dwCertEncodingType,
         0, CERT_FIND_SHA1_HASH, &blob, NULL);
    }
    return matching;
}

static BOOL CRYPT_CheckRestrictedRoot(HCERTSTORE store)
{
    BOOL ret = TRUE;

    if (store)
    {
        HCERTSTORE rootStore = CertOpenSystemStoreW(0, rootW);
        PCCERT_CONTEXT cert = NULL, check;

        do {
            cert = CertEnumCertificatesInStore(store, cert);
            if (cert)
            {
                if (!(check = CRYPT_FindCertInStore(rootStore, cert)))
                    ret = FALSE;
                else
                    CertFreeCertificateContext(check);
            }
        } while (ret && cert);
        if (cert)
            CertFreeCertificateContext(cert);
        CertCloseStore(rootStore, 0);
    }
    return ret;
}

HCERTCHAINENGINE CRYPT_CreateChainEngine(HCERTSTORE root,
 PCERT_CHAIN_ENGINE_CONFIG pConfig)
{
    static const WCHAR caW[] = { 'C','A',0 };
    static const WCHAR myW[] = { 'M','y',0 };
    static const WCHAR trustW[] = { 'T','r','u','s','t',0 };
    PCertificateChainEngine engine =
     CryptMemAlloc(sizeof(CertificateChainEngine));

    if (engine)
    {
        HCERTSTORE worldStores[4];

        engine->ref = 1;
        engine->hRoot = root;
        engine->hWorld = CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0,
         CERT_STORE_CREATE_NEW_FLAG, NULL);
        worldStores[0] = CertDuplicateStore(engine->hRoot);
        worldStores[1] = CertOpenSystemStoreW(0, caW);
        worldStores[2] = CertOpenSystemStoreW(0, myW);
        worldStores[3] = CertOpenSystemStoreW(0, trustW);
        CRYPT_AddStoresToCollection(engine->hWorld,
         sizeof(worldStores) / sizeof(worldStores[0]), worldStores);
        CRYPT_AddStoresToCollection(engine->hWorld,
         pConfig->cAdditionalStore, pConfig->rghAdditionalStore);
        CRYPT_CloseStores(sizeof(worldStores) / sizeof(worldStores[0]),
         worldStores);
        engine->dwFlags = pConfig->dwFlags;
        engine->dwUrlRetrievalTimeout = pConfig->dwUrlRetrievalTimeout;
        engine->MaximumCachedCertificates =
         pConfig->MaximumCachedCertificates;
        if (pConfig->CycleDetectionModulus)
            engine->CycleDetectionModulus = pConfig->CycleDetectionModulus;
        else
            engine->CycleDetectionModulus = DEFAULT_CYCLE_MODULUS;
    }
    return engine;
}

BOOL WINAPI CertCreateCertificateChainEngine(PCERT_CHAIN_ENGINE_CONFIG pConfig,
 HCERTCHAINENGINE *phChainEngine)
{
    BOOL ret;

    TRACE("(%p, %p)\n", pConfig, phChainEngine);

    if (pConfig->cbSize != sizeof(*pConfig))
    {
        SetLastError(E_INVALIDARG);
        return FALSE;
    }
    *phChainEngine = NULL;
    ret = CRYPT_CheckRestrictedRoot(pConfig->hRestrictedRoot);
    if (ret)
    {
        HCERTSTORE root;
        HCERTCHAINENGINE engine;

        if (pConfig->hRestrictedRoot)
            root = CertDuplicateStore(pConfig->hRestrictedRoot);
        else
            root = CertOpenSystemStoreW(0, rootW);
        engine = CRYPT_CreateChainEngine(root, pConfig);
        if (engine)
        {
            *phChainEngine = engine;
            ret = TRUE;
        }
        else
            ret = FALSE;
    }
    return ret;
}

VOID WINAPI CertFreeCertificateChainEngine(HCERTCHAINENGINE hChainEngine)
{
    PCertificateChainEngine engine = (PCertificateChainEngine)hChainEngine;

    TRACE("(%p)\n", hChainEngine);

    if (engine && InterlockedDecrement(&engine->ref) == 0)
    {
        CertCloseStore(engine->hWorld, 0);
        CertCloseStore(engine->hRoot, 0);
        CryptMemFree(engine);
    }
}

static HCERTCHAINENGINE CRYPT_GetDefaultChainEngine(void)
{
    if (!CRYPT_defaultChainEngine)
    {
        CERT_CHAIN_ENGINE_CONFIG config = { 0 };
        HCERTCHAINENGINE engine;

        config.cbSize = sizeof(config);
        CertCreateCertificateChainEngine(&config, &engine);
        InterlockedCompareExchangePointer(&CRYPT_defaultChainEngine, engine,
         NULL);
        if (CRYPT_defaultChainEngine != engine)
            CertFreeCertificateChainEngine(engine);
    }
    return CRYPT_defaultChainEngine;
}

void default_chain_engine_free(void)
{
    CertFreeCertificateChainEngine(CRYPT_defaultChainEngine);
}

typedef struct _CertificateChain
{
    CERT_CHAIN_CONTEXT context;
    HCERTSTORE world;
    LONG ref;
} CertificateChain, *PCertificateChain;

static inline BOOL CRYPT_IsCertificateSelfSigned(PCCERT_CONTEXT cert)
{
    return CertCompareCertificateName(cert->dwCertEncodingType,
     &cert->pCertInfo->Subject, &cert->pCertInfo->Issuer);
}

static void CRYPT_FreeChainElement(PCERT_CHAIN_ELEMENT element)
{
    CertFreeCertificateContext(element->pCertContext);
    CryptMemFree(element);
}

static void CRYPT_CheckSimpleChainForCycles(PCERT_SIMPLE_CHAIN chain)
{
    DWORD i, j, cyclicCertIndex = 0;

    /* O(n^2) - I don't think there's a faster way */
    for (i = 0; !cyclicCertIndex && i < chain->cElement; i++)
        for (j = i + 1; !cyclicCertIndex && j < chain->cElement; j++)
            if (CertCompareCertificate(X509_ASN_ENCODING,
             chain->rgpElement[i]->pCertContext->pCertInfo,
             chain->rgpElement[j]->pCertContext->pCertInfo))
                cyclicCertIndex = j;
    if (cyclicCertIndex)
    {
        chain->rgpElement[cyclicCertIndex]->TrustStatus.dwErrorStatus
         |= CERT_TRUST_IS_CYCLIC | CERT_TRUST_INVALID_BASIC_CONSTRAINTS;
        /* Release remaining certs */
        for (i = cyclicCertIndex + 1; i < chain->cElement; i++)
            CRYPT_FreeChainElement(chain->rgpElement[i]);
        /* Truncate chain */
        chain->cElement = cyclicCertIndex + 1;
    }
}

/* Checks whether the chain is cyclic by examining the last element's status */
static inline BOOL CRYPT_IsSimpleChainCyclic(const CERT_SIMPLE_CHAIN *chain)
{
    if (chain->cElement)
        return chain->rgpElement[chain->cElement - 1]->TrustStatus.dwErrorStatus
         & CERT_TRUST_IS_CYCLIC;
    else
        return FALSE;
}

static inline void CRYPT_CombineTrustStatus(CERT_TRUST_STATUS *chainStatus,
 const CERT_TRUST_STATUS *elementStatus)
{
    /* Any error that applies to an element also applies to a chain.. */
    chainStatus->dwErrorStatus |= elementStatus->dwErrorStatus;
    /* but the bottom nibble of an element's info status doesn't apply to the
     * chain.
     */
    chainStatus->dwInfoStatus |= (elementStatus->dwInfoStatus & 0xfffffff0);
}

static BOOL CRYPT_AddCertToSimpleChain(const CertificateChainEngine *engine,
 PCERT_SIMPLE_CHAIN chain, PCCERT_CONTEXT cert, DWORD subjectInfoStatus)
{
    BOOL ret = FALSE;
    PCERT_CHAIN_ELEMENT element = CryptMemAlloc(sizeof(CERT_CHAIN_ELEMENT));

    if (element)
    {
        if (!chain->cElement)
            chain->rgpElement = CryptMemAlloc(sizeof(PCERT_CHAIN_ELEMENT));
        else
            chain->rgpElement = CryptMemRealloc(chain->rgpElement,
             (chain->cElement + 1) * sizeof(PCERT_CHAIN_ELEMENT));
        if (chain->rgpElement)
        {
            chain->rgpElement[chain->cElement++] = element;
            memset(element, 0, sizeof(CERT_CHAIN_ELEMENT));
            element->cbSize = sizeof(CERT_CHAIN_ELEMENT);
            element->pCertContext = CertDuplicateCertificateContext(cert);
            if (chain->cElement > 1)
                chain->rgpElement[chain->cElement - 2]->TrustStatus.dwInfoStatus
                 = subjectInfoStatus;
            /* FIXME: initialize the rest of element */
            if (!(chain->cElement % engine->CycleDetectionModulus))
            {
                CRYPT_CheckSimpleChainForCycles(chain);
                /* Reinitialize the element pointer in case the chain is
                 * cyclic, in which case the chain is truncated.
                 */
                element = chain->rgpElement[chain->cElement - 1];
            }
            CRYPT_CombineTrustStatus(&chain->TrustStatus,
             &element->TrustStatus);
            ret = TRUE;
        }
        else
            CryptMemFree(element);
    }
    return ret;
}

static void CRYPT_FreeSimpleChain(PCERT_SIMPLE_CHAIN chain)
{
    DWORD i;

    for (i = 0; i < chain->cElement; i++)
        CRYPT_FreeChainElement(chain->rgpElement[i]);
    CryptMemFree(chain->rgpElement);
    CryptMemFree(chain);
}

static void CRYPT_CheckTrustedStatus(HCERTSTORE hRoot,
 PCERT_CHAIN_ELEMENT rootElement)
{
    PCCERT_CONTEXT trustedRoot = CRYPT_FindCertInStore(hRoot,
     rootElement->pCertContext);

    if (!trustedRoot)
        rootElement->TrustStatus.dwErrorStatus |=
         CERT_TRUST_IS_UNTRUSTED_ROOT;
    else
        CertFreeCertificateContext(trustedRoot);
}

static void CRYPT_CheckRootCert(HCERTCHAINENGINE hRoot,
 PCERT_CHAIN_ELEMENT rootElement)
{
    PCCERT_CONTEXT root = rootElement->pCertContext;

    if (!CryptVerifyCertificateSignatureEx(0, root->dwCertEncodingType,
     CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT, (void *)root,
     CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)root, 0, NULL))
    {
        TRACE_(chain)("Last certificate's signature is invalid\n");
        rootElement->TrustStatus.dwErrorStatus |=
         CERT_TRUST_IS_NOT_SIGNATURE_VALID;
    }
    CRYPT_CheckTrustedStatus(hRoot, rootElement);
}

/* Decodes a cert's basic constraints extension (either szOID_BASIC_CONSTRAINTS
 * or szOID_BASIC_CONSTRAINTS2, whichever is present) into a
 * CERT_BASIC_CONSTRAINTS2_INFO.  If it neither extension is present, sets
 * constraints->fCA to defaultIfNotSpecified.
 * Returns FALSE if the extension is present but couldn't be decoded.
 */
static BOOL CRYPT_DecodeBasicConstraints(PCCERT_CONTEXT cert,
 CERT_BASIC_CONSTRAINTS2_INFO *constraints, BOOL defaultIfNotSpecified)
{
    BOOL ret = TRUE;
    PCERT_EXTENSION ext = CertFindExtension(szOID_BASIC_CONSTRAINTS,
     cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);

    constraints->fPathLenConstraint = FALSE;
    if (ext)
    {
        CERT_BASIC_CONSTRAINTS_INFO *info;
        DWORD size = 0;

        ret = CryptDecodeObjectEx(X509_ASN_ENCODING, szOID_BASIC_CONSTRAINTS,
         ext->Value.pbData, ext->Value.cbData, CRYPT_DECODE_ALLOC_FLAG,
         NULL, &info, &size);
        if (ret)
        {
            if (info->SubjectType.cbData == 1)
                constraints->fCA =
                 info->SubjectType.pbData[0] & CERT_CA_SUBJECT_FLAG;
            LocalFree(info);
        }
    }
    else
    {
        ext = CertFindExtension(szOID_BASIC_CONSTRAINTS2,
         cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);
        if (ext)
        {
            DWORD size = sizeof(CERT_BASIC_CONSTRAINTS2_INFO);

            ret = CryptDecodeObjectEx(X509_ASN_ENCODING,
             szOID_BASIC_CONSTRAINTS2, ext->Value.pbData, ext->Value.cbData,
             0, NULL, constraints, &size);
        }
        else
            constraints->fCA = defaultIfNotSpecified;
    }
    return ret;
}

/* Checks element's basic constraints to see if it can act as a CA, with
 * remainingCAs CAs left in this chain.  In general, a cert must include the
 * basic constraints extension, with the CA flag asserted, in order to be
 * allowed to be a CA.  A V1 or V2 cert, which has no extensions, is also
 * allowed to be a CA if it's installed locally (in the engine's world store.)
 * This matches the expected usage in RFC 5280, section 4.2.1.9:  a conforming
 * CA MUST include the basic constraints extension in all certificates that are
 * used to validate digital signatures on certificates.  It also matches
 * section 6.1.4(k): "If a certificate is a v1 or v2 certificate, then the
 * application MUST either verify that the certificate is a CA certificate
 * through out-of-band means or reject the certificate." Rejecting the
 * certificate prohibits a large number of commonly used certificates, so
 * accepting locally installed ones is a compromise.
 * Root certificates are also allowed to be CAs even without a basic
 * constraints extension.  This is implied by RFC 5280, section 6.1:  the
 * root of a certificate chain's only requirement is that it was used to issue
 * the next certificate in the chain.
 * Updates chainConstraints with the element's constraints, if:
 * 1. chainConstraints doesn't have a path length constraint, or
 * 2. element's path length constraint is smaller than chainConstraints's
 * Sets *pathLengthConstraintViolated to TRUE if a path length violation
 * occurs.
 * Returns TRUE if the element can be a CA, and the length of the remaining
 * chain is valid.
 */
static BOOL CRYPT_CheckBasicConstraintsForCA(PCertificateChainEngine engine,
 PCCERT_CONTEXT cert, CERT_BASIC_CONSTRAINTS2_INFO *chainConstraints,
 DWORD remainingCAs, BOOL isRoot, BOOL *pathLengthConstraintViolated)
{
    BOOL validBasicConstraints, implicitCA = FALSE;
    CERT_BASIC_CONSTRAINTS2_INFO constraints;

    if (isRoot)
        implicitCA = TRUE;
    else if (cert->pCertInfo->dwVersion == CERT_V1 ||
     cert->pCertInfo->dwVersion == CERT_V2)
    {
        BYTE hash[20];
        DWORD size = sizeof(hash);

        if (CertGetCertificateContextProperty(cert, CERT_HASH_PROP_ID,
         hash, &size))
        {
            CRYPT_HASH_BLOB blob = { sizeof(hash), hash };
            PCCERT_CONTEXT localCert = CertFindCertificateInStore(
             engine->hWorld, cert->dwCertEncodingType, 0, CERT_FIND_SHA1_HASH,
             &blob, NULL);

            if (localCert)
            {
                CertFreeCertificateContext(localCert);
                implicitCA = TRUE;
            }
        }
    }
    if ((validBasicConstraints = CRYPT_DecodeBasicConstraints(cert,
     &constraints, implicitCA)))
    {
        chainConstraints->fCA = constraints.fCA;
        if (!constraints.fCA)
        {
            TRACE_(chain)("chain element %d can't be a CA\n", remainingCAs + 1);
            validBasicConstraints = FALSE;
        }
        else if (constraints.fPathLenConstraint)
        {
            /* If the element has path length constraints, they apply to the
             * entire remaining chain.
             */
            if (!chainConstraints->fPathLenConstraint ||
             constraints.dwPathLenConstraint <
             chainConstraints->dwPathLenConstraint)
            {
                TRACE_(chain)("setting path length constraint to %d\n",
                 chainConstraints->dwPathLenConstraint);
                chainConstraints->fPathLenConstraint = TRUE;
                chainConstraints->dwPathLenConstraint =
                 constraints.dwPathLenConstraint;
            }
        }
    }
    if (chainConstraints->fPathLenConstraint &&
     remainingCAs > chainConstraints->dwPathLenConstraint)
    {
        TRACE_(chain)("remaining CAs %d exceed max path length %d\n",
         remainingCAs, chainConstraints->dwPathLenConstraint);
        validBasicConstraints = FALSE;
        *pathLengthConstraintViolated = TRUE;
    }
    return validBasicConstraints;
}

static BOOL url_matches(LPCWSTR constraint, LPCWSTR name,
 DWORD *trustErrorStatus)
{
    BOOL match = FALSE;

    TRACE("%s, %s\n", debugstr_w(constraint), debugstr_w(name));

    if (!constraint)
        *trustErrorStatus |= CERT_TRUST_INVALID_NAME_CONSTRAINTS;
    else if (!name)
        ; /* no match */
    else if (constraint[0] == '.')
    {
        if (lstrlenW(name) > lstrlenW(constraint))
            match = !lstrcmpiW(name + lstrlenW(name) - lstrlenW(constraint),
             constraint);
    }
    else
        match = !lstrcmpiW(constraint, name);
    return match;
}

static BOOL rfc822_name_matches(LPCWSTR constraint, LPCWSTR name,
 DWORD *trustErrorStatus)
{
    BOOL match = FALSE;
    LPCWSTR at;

    TRACE("%s, %s\n", debugstr_w(constraint), debugstr_w(name));

    if (!constraint)
        *trustErrorStatus |= CERT_TRUST_INVALID_NAME_CONSTRAINTS;
    else if (!name)
        ; /* no match */
    else if ((at = strchrW(constraint, '@')))
        match = !lstrcmpiW(constraint, name);
    else
    {
        if ((at = strchrW(name, '@')))
            match = url_matches(constraint, at + 1, trustErrorStatus);
        else
            match = !lstrcmpiW(constraint, name);
    }
    return match;
}

static BOOL dns_name_matches(LPCWSTR constraint, LPCWSTR name,
 DWORD *trustErrorStatus)
{
    BOOL match = FALSE;

    TRACE("%s, %s\n", debugstr_w(constraint), debugstr_w(name));

    if (!constraint)
        *trustErrorStatus |= CERT_TRUST_INVALID_NAME_CONSTRAINTS;
    else if (!name)
        ; /* no match */
    else if (lstrlenW(name) >= lstrlenW(constraint))
        match = !lstrcmpiW(name + lstrlenW(name) - lstrlenW(constraint),
         constraint);
    /* else:  name is too short, no match */

    return match;
}

static BOOL ip_address_matches(const CRYPT_DATA_BLOB *constraint,
 const CRYPT_DATA_BLOB *name, DWORD *trustErrorStatus)
{
    BOOL match = FALSE;

    TRACE("(%d, %p), (%d, %p)\n", constraint->cbData, constraint->pbData,
     name->cbData, name->pbData);

    /* RFC5280, section 4.2.1.10, iPAddress syntax: either 8 or 32 bytes, for
     * IPv4 or IPv6 addresses, respectively.
     */
    if (constraint->cbData != sizeof(DWORD) * 2 && constraint->cbData != 32)
        *trustErrorStatus |= CERT_TRUST_INVALID_NAME_CONSTRAINTS;
    else if (name->cbData == sizeof(DWORD) &&
     constraint->cbData == sizeof(DWORD) * 2)
    {
        DWORD subnet, mask, addr;

        memcpy(&subnet, constraint->pbData, sizeof(subnet));
        memcpy(&mask, constraint->pbData + sizeof(subnet), sizeof(mask));
        memcpy(&addr, name->pbData, sizeof(addr));
        /* These are really in big-endian order, but for equality matching we
         * don't need to swap to host order
         */
        match = (subnet & mask) == (addr & mask);
    }
    else if (name->cbData == 16 && constraint->cbData == 32)
    {
        const BYTE *subnet, *mask, *addr;
        DWORD i;

        subnet = constraint->pbData;
        mask = constraint->pbData + 16;
        addr = name->pbData;
        match = TRUE;
        for (i = 0; match && i < 16; i++)
            if ((subnet[i] & mask[i]) != (addr[i] & mask[i]))
                match = FALSE;
    }
    /* else: name is wrong size, no match */

    return match;
}

static void CRYPT_FindMatchingNameEntry(const CERT_ALT_NAME_ENTRY *constraint,
 const CERT_ALT_NAME_INFO *subjectName, DWORD *trustErrorStatus,
 DWORD errorIfFound, DWORD errorIfNotFound)
{
    DWORD i;
    BOOL match = FALSE;

    for (i = 0; i < subjectName->cAltEntry; i++)
    {
        if (subjectName->rgAltEntry[i].dwAltNameChoice ==
         constraint->dwAltNameChoice)
        {
            switch (constraint->dwAltNameChoice)
            {
            case CERT_ALT_NAME_RFC822_NAME:
                match = rfc822_name_matches(constraint->u.pwszURL,
                 subjectName->rgAltEntry[i].u.pwszURL, trustErrorStatus);
                break;
            case CERT_ALT_NAME_DNS_NAME:
                match = dns_name_matches(constraint->u.pwszURL,
                 subjectName->rgAltEntry[i].u.pwszURL, trustErrorStatus);
                break;
            case CERT_ALT_NAME_URL:
                match = url_matches(constraint->u.pwszURL,
                 subjectName->rgAltEntry[i].u.pwszURL, trustErrorStatus);
                break;
            case CERT_ALT_NAME_IP_ADDRESS:
                match = ip_address_matches(&constraint->u.IPAddress,
                 &subjectName->rgAltEntry[i].u.IPAddress, trustErrorStatus);
                break;
            case CERT_ALT_NAME_DIRECTORY_NAME:
            default:
                ERR("name choice %d unsupported in this context\n",
                 constraint->dwAltNameChoice);
                *trustErrorStatus |=
                 CERT_TRUST_HAS_NOT_SUPPORTED_NAME_CONSTRAINT;
            }
        }
    }
    *trustErrorStatus |= match ? errorIfFound : errorIfNotFound;
}

static void CRYPT_CheckNameConstraints(
 const CERT_NAME_CONSTRAINTS_INFO *nameConstraints, const CERT_INFO *cert,
 DWORD *trustErrorStatus)
{
    /* If there aren't any existing constraints, don't bother checking */
    if (nameConstraints->cPermittedSubtree || nameConstraints->cExcludedSubtree)
    {
        CERT_EXTENSION *ext;

        ext = CertFindExtension(szOID_SUBJECT_ALT_NAME2, cert->cExtension,
         cert->rgExtension);
        if (!ext)
            ext = CertFindExtension(szOID_SUBJECT_ALT_NAME, cert->cExtension,
             cert->rgExtension);
        if (ext)
        {
            CERT_ALT_NAME_INFO *subjectName;
            DWORD size;

            if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME,
             ext->Value.pbData, ext->Value.cbData,
             CRYPT_DECODE_ALLOC_FLAG | CRYPT_DECODE_NOCOPY_FLAG, NULL,
             &subjectName, &size))
            {
                DWORD i;

                for (i = 0; i < nameConstraints->cExcludedSubtree; i++)
                    CRYPT_FindMatchingNameEntry(
                     &nameConstraints->rgExcludedSubtree[i].Base, subjectName,
                     trustErrorStatus,
                     CERT_TRUST_HAS_EXCLUDED_NAME_CONSTRAINT, 0);
                for (i = 0; i < nameConstraints->cPermittedSubtree; i++)
                    CRYPT_FindMatchingNameEntry(
                     &nameConstraints->rgPermittedSubtree[i].Base, subjectName,
                     trustErrorStatus,
                     0, CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT);
                LocalFree(subjectName);
            }
            else
                *trustErrorStatus |=
                 CERT_TRUST_INVALID_EXTENSION |
                 CERT_TRUST_INVALID_NAME_CONSTRAINTS;
        }
        else
        {
            if (nameConstraints->cPermittedSubtree)
                *trustErrorStatus |=
                 CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT;
            if (nameConstraints->cExcludedSubtree)
                *trustErrorStatus |=
                 CERT_TRUST_HAS_EXCLUDED_NAME_CONSTRAINT;
        }
    }
}

/* Gets cert's name constraints, if any.  Free with LocalFree. */
static CERT_NAME_CONSTRAINTS_INFO *CRYPT_GetNameConstraints(CERT_INFO *cert)
{
    CERT_NAME_CONSTRAINTS_INFO *info = NULL;

    CERT_EXTENSION *ext;

    if ((ext = CertFindExtension(szOID_NAME_CONSTRAINTS, cert->cExtension,
     cert->rgExtension)))
    {
        DWORD size;

        CryptDecodeObjectEx(X509_ASN_ENCODING, X509_NAME_CONSTRAINTS,
         ext->Value.pbData, ext->Value.cbData,
         CRYPT_DECODE_ALLOC_FLAG | CRYPT_DECODE_NOCOPY_FLAG, NULL, &info,
         &size);
    }
    return info;
}

static BOOL CRYPT_IsValidNameConstraint(const CERT_NAME_CONSTRAINTS_INFO *info)
{
    DWORD i;
    BOOL ret = TRUE;

    /* Check that none of the constraints specifies a minimum or a maximum.
     * See RFC 5280, section 4.2.1.10:
     * "Within this profile, the minimum and maximum fields are not used with
     *  any name forms, thus, the minimum MUST be zero, and maximum MUST be
     *  absent.  However, if an application encounters a critical name
     *  constraints extension that specifies other values for minimum or
     *  maximum for a name form that appears in a subsequent certificate, the
     *  application MUST either process these fields or reject the
     *  certificate."
     * Since it gives no guidance as to how to process these fields, we
     * reject any name constraint that contains them.
     */
    for (i = 0; ret && i < info->cPermittedSubtree; i++)
        if (info->rgPermittedSubtree[i].dwMinimum ||
         info->rgPermittedSubtree[i].fMaximum)
        {
            TRACE_(chain)("found a minimum or maximum in permitted subtrees\n");
            ret = FALSE;
        }
    for (i = 0; ret && i < info->cExcludedSubtree; i++)
        if (info->rgExcludedSubtree[i].dwMinimum ||
         info->rgExcludedSubtree[i].fMaximum)
        {
            TRACE_(chain)("found a minimum or maximum in excluded subtrees\n");
            ret = FALSE;
        }
    return ret;
}

static void CRYPT_CheckChainNameConstraints(PCERT_SIMPLE_CHAIN chain)
{
    int i, j;

    /* Microsoft's implementation appears to violate RFC 3280:  according to
     * MSDN, the various CERT_TRUST_*_NAME_CONSTRAINT errors are set if a CA's
     * name constraint is violated in the end cert.  According to RFC 3280,
     * the constraints should be checked against every subsequent certificate
     * in the chain, not just the end cert.
     * Microsoft's implementation also sets the name constraint errors on the
     * certs whose constraints were violated, not on the certs that violated
     * them.
     * In order to be error-compatible with Microsoft's implementation, while
     * still adhering to RFC 3280, I use a O(n ^ 2) algorithm to check name
     * constraints.
     */
    for (i = chain->cElement - 1; i > 0; i--)
    {
        CERT_NAME_CONSTRAINTS_INFO *nameConstraints;

        if ((nameConstraints = CRYPT_GetNameConstraints(
         chain->rgpElement[i]->pCertContext->pCertInfo)))
        {
            if (!CRYPT_IsValidNameConstraint(nameConstraints))
                chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
                 CERT_TRUST_HAS_NOT_SUPPORTED_NAME_CONSTRAINT;
            else
            {
                for (j = i - 1; j >= 0; j--)
                {
                    DWORD errorStatus = 0;

                    /* According to RFC 3280, self-signed certs don't have name
                     * constraints checked unless they're the end cert.
                     */
                    if (j == 0 || !CRYPT_IsCertificateSelfSigned(
                     chain->rgpElement[j]->pCertContext))
                    {
                        CRYPT_CheckNameConstraints(nameConstraints,
                         chain->rgpElement[j]->pCertContext->pCertInfo,
                         &errorStatus);
                        chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
                         errorStatus;
                    }
                }
            }
            LocalFree(nameConstraints);
        }
    }
}

static LPWSTR name_value_to_str(const CERT_NAME_BLOB *name)
{
    DWORD len = cert_name_to_str_with_indent(X509_ASN_ENCODING, 0, name,
     CERT_SIMPLE_NAME_STR, NULL, 0);
    LPWSTR str = NULL;

    if (len)
    {
        str = CryptMemAlloc(len * sizeof(WCHAR));
        if (str)
            cert_name_to_str_with_indent(X509_ASN_ENCODING, 0, name,
             CERT_SIMPLE_NAME_STR, str, len);
    }
    return str;
}

static void dump_alt_name_entry(const CERT_ALT_NAME_ENTRY *entry)
{
    LPWSTR str;

    switch (entry->dwAltNameChoice)
    {
    case CERT_ALT_NAME_OTHER_NAME:
        TRACE_(chain)("CERT_ALT_NAME_OTHER_NAME, oid = %s\n",
         debugstr_a(entry->u.pOtherName->pszObjId));
         break;
    case CERT_ALT_NAME_RFC822_NAME:
        TRACE_(chain)("CERT_ALT_NAME_RFC822_NAME: %s\n",
         debugstr_w(entry->u.pwszRfc822Name));
        break;
    case CERT_ALT_NAME_DNS_NAME:
        TRACE_(chain)("CERT_ALT_NAME_DNS_NAME: %s\n",
         debugstr_w(entry->u.pwszDNSName));
        break;
    case CERT_ALT_NAME_DIRECTORY_NAME:
        str = name_value_to_str(&entry->u.DirectoryName);
        TRACE_(chain)("CERT_ALT_NAME_DIRECTORY_NAME: %s\n", debugstr_w(str));
        CryptMemFree(str);
        break;
    case CERT_ALT_NAME_URL:
        TRACE_(chain)("CERT_ALT_NAME_URL: %s\n", debugstr_w(entry->u.pwszURL));
        break;
    case CERT_ALT_NAME_IP_ADDRESS:
        TRACE_(chain)("CERT_ALT_NAME_IP_ADDRESS: %d bytes\n",
         entry->u.IPAddress.cbData);
        break;
    case CERT_ALT_NAME_REGISTERED_ID:
        TRACE_(chain)("CERT_ALT_NAME_REGISTERED_ID: %s\n",
         debugstr_a(entry->u.pszRegisteredID));
        break;
    default:
        TRACE_(chain)("dwAltNameChoice = %d\n", entry->dwAltNameChoice);
    }
}

static void dump_alt_name(LPCSTR type, const CERT_EXTENSION *ext)
{
    CERT_ALT_NAME_INFO *name;
    DWORD size;

    TRACE_(chain)("%s:\n", type);
    if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME,
     ext->Value.pbData, ext->Value.cbData,
     CRYPT_DECODE_ALLOC_FLAG | CRYPT_DECODE_NOCOPY_FLAG, NULL, &name, &size))
    {
        DWORD i;

        TRACE_(chain)("%d alt name entries:\n", name->cAltEntry);
        for (i = 0; i < name->cAltEntry; i++)
            dump_alt_name_entry(&name->rgAltEntry[i]);
        LocalFree(name);
    }
}

static void dump_basic_constraints(const CERT_EXTENSION *ext)
{
    CERT_BASIC_CONSTRAINTS_INFO *info;
    DWORD size = 0;

    if (CryptDecodeObjectEx(X509_ASN_ENCODING, szOID_BASIC_CONSTRAINTS,
     ext->Value.pbData, ext->Value.cbData, CRYPT_DECODE_ALLOC_FLAG,
     NULL, &info, &size))
    {
        TRACE_(chain)("SubjectType: %02x\n", info->SubjectType.pbData[0]);
        TRACE_(chain)("%s path length constraint\n",
         info->fPathLenConstraint ? "has" : "doesn't have");
        TRACE_(chain)("path length=%d\n", info->dwPathLenConstraint);
        LocalFree(info);
    }
}

static void dump_basic_constraints2(const CERT_EXTENSION *ext)
{
    CERT_BASIC_CONSTRAINTS2_INFO constraints;
    DWORD size = sizeof(CERT_BASIC_CONSTRAINTS2_INFO);

    if (CryptDecodeObjectEx(X509_ASN_ENCODING,
     szOID_BASIC_CONSTRAINTS2, ext->Value.pbData, ext->Value.cbData,
     0, NULL, &constraints, &size))
    {
        TRACE_(chain)("basic constraints:\n");
        TRACE_(chain)("can%s be a CA\n", constraints.fCA ? "" : "not");
        TRACE_(chain)("%s path length constraint\n",
         constraints.fPathLenConstraint ? "has" : "doesn't have");
        TRACE_(chain)("path length=%d\n", constraints.dwPathLenConstraint);
    }
}

static void dump_key_usage(const CERT_EXTENSION *ext)
{
    CRYPT_BIT_BLOB usage;
    DWORD size = sizeof(usage);

    if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_BITS, ext->Value.pbData,
     ext->Value.cbData, CRYPT_DECODE_NOCOPY_FLAG, NULL, &usage, &size))
    {
#define trace_usage_bit(bits, bit) \
 if ((bits) & (bit)) TRACE_(chain)("%s\n", #bit)
        if (usage.cbData)
        {
            trace_usage_bit(usage.pbData[0], CERT_DIGITAL_SIGNATURE_KEY_USAGE);
            trace_usage_bit(usage.pbData[0], CERT_NON_REPUDIATION_KEY_USAGE);
            trace_usage_bit(usage.pbData[0], CERT_KEY_ENCIPHERMENT_KEY_USAGE);
            trace_usage_bit(usage.pbData[0], CERT_DATA_ENCIPHERMENT_KEY_USAGE);
            trace_usage_bit(usage.pbData[0], CERT_KEY_AGREEMENT_KEY_USAGE);
            trace_usage_bit(usage.pbData[0], CERT_KEY_CERT_SIGN_KEY_USAGE);
            trace_usage_bit(usage.pbData[0], CERT_CRL_SIGN_KEY_USAGE);
            trace_usage_bit(usage.pbData[0], CERT_ENCIPHER_ONLY_KEY_USAGE);
        }
#undef trace_usage_bit
        if (usage.cbData > 1 && usage.pbData[1] & CERT_DECIPHER_ONLY_KEY_USAGE)
            TRACE_(chain)("CERT_DECIPHER_ONLY_KEY_USAGE\n");
    }
}

static void dump_general_subtree(const CERT_GENERAL_SUBTREE *subtree)
{
    dump_alt_name_entry(&subtree->Base);
    TRACE_(chain)("dwMinimum = %d, fMaximum = %d, dwMaximum = %d\n",
     subtree->dwMinimum, subtree->fMaximum, subtree->dwMaximum);
}

static void dump_name_constraints(const CERT_EXTENSION *ext)
{
    CERT_NAME_CONSTRAINTS_INFO *nameConstraints;
    DWORD size;

    if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_NAME_CONSTRAINTS,
     ext->Value.pbData, ext->Value.cbData,
     CRYPT_DECODE_ALLOC_FLAG | CRYPT_DECODE_NOCOPY_FLAG, NULL, &nameConstraints,
     &size))
    {
        DWORD i;

        TRACE_(chain)("%d permitted subtrees:\n",
         nameConstraints->cPermittedSubtree);
        for (i = 0; i < nameConstraints->cPermittedSubtree; i++)
            dump_general_subtree(&nameConstraints->rgPermittedSubtree[i]);
        TRACE_(chain)("%d excluded subtrees:\n",
         nameConstraints->cExcludedSubtree);
        for (i = 0; i < nameConstraints->cExcludedSubtree; i++)
            dump_general_subtree(&nameConstraints->rgExcludedSubtree[i]);
        LocalFree(nameConstraints);
    }
}

static void dump_cert_policies(const CERT_EXTENSION *ext)
{
    CERT_POLICIES_INFO *policies;
    DWORD size;

    if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_CERT_POLICIES,
     ext->Value.pbData, ext->Value.cbData, CRYPT_DECODE_ALLOC_FLAG, NULL,
     &policies, &size))
    {
        DWORD i, j;

        TRACE_(chain)("%d policies:\n", policies->cPolicyInfo);
        for (i = 0; i < policies->cPolicyInfo; i++)
        {
            TRACE_(chain)("policy identifier: %s\n",
             debugstr_a(policies->rgPolicyInfo[i].pszPolicyIdentifier));
            TRACE_(chain)("%d policy qualifiers:\n",
             policies->rgPolicyInfo[i].cPolicyQualifier);
            for (j = 0; j < policies->rgPolicyInfo[i].cPolicyQualifier; j++)
                TRACE_(chain)("%s\n", debugstr_a(
                 policies->rgPolicyInfo[i].rgPolicyQualifier[j].
                 pszPolicyQualifierId));
        }
        LocalFree(policies);
    }
}

static void dump_enhanced_key_usage(const CERT_EXTENSION *ext)
{
    CERT_ENHKEY_USAGE *usage;
    DWORD size;

    if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE,
     ext->Value.pbData, ext->Value.cbData, CRYPT_DECODE_ALLOC_FLAG, NULL,
     &usage, &size))
    {
        DWORD i;

        TRACE_(chain)("%d usages:\n", usage->cUsageIdentifier);
        for (i = 0; i < usage->cUsageIdentifier; i++)
            TRACE_(chain)("%s\n", usage->rgpszUsageIdentifier[i]);
        LocalFree(usage);
    }
}

static void dump_netscape_cert_type(const CERT_EXTENSION *ext)
{
    CRYPT_BIT_BLOB usage;
    DWORD size = sizeof(usage);

    if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_BITS, ext->Value.pbData,
     ext->Value.cbData, CRYPT_DECODE_NOCOPY_FLAG, NULL, &usage, &size))
    {
#define trace_cert_type_bit(bits, bit) \
 if ((bits) & (bit)) TRACE_(chain)("%s\n", #bit)
        if (usage.cbData)
        {
            trace_cert_type_bit(usage.pbData[0],
             NETSCAPE_SSL_CLIENT_AUTH_CERT_TYPE);
            trace_cert_type_bit(usage.pbData[0],
             NETSCAPE_SSL_SERVER_AUTH_CERT_TYPE);
            trace_cert_type_bit(usage.pbData[0], NETSCAPE_SMIME_CERT_TYPE);
            trace_cert_type_bit(usage.pbData[0], NETSCAPE_SIGN_CERT_TYPE);
            trace_cert_type_bit(usage.pbData[0], NETSCAPE_SSL_CA_CERT_TYPE);
            trace_cert_type_bit(usage.pbData[0], NETSCAPE_SMIME_CA_CERT_TYPE);
            trace_cert_type_bit(usage.pbData[0], NETSCAPE_SIGN_CA_CERT_TYPE);
        }
#undef trace_cert_type_bit
    }
}

static void dump_extension(const CERT_EXTENSION *ext)
{
    TRACE_(chain)("%s (%scritical)\n", debugstr_a(ext->pszObjId),
     ext->fCritical ? "" : "not ");
    if (!strcmp(ext->pszObjId, szOID_SUBJECT_ALT_NAME))
        dump_alt_name("subject alt name", ext);
    else  if (!strcmp(ext->pszObjId, szOID_ISSUER_ALT_NAME))
        dump_alt_name("issuer alt name", ext);
    else if (!strcmp(ext->pszObjId, szOID_BASIC_CONSTRAINTS))
        dump_basic_constraints(ext);
    else if (!strcmp(ext->pszObjId, szOID_KEY_USAGE))
        dump_key_usage(ext);
    else if (!strcmp(ext->pszObjId, szOID_SUBJECT_ALT_NAME2))
        dump_alt_name("subject alt name 2", ext);
    else if (!strcmp(ext->pszObjId, szOID_ISSUER_ALT_NAME2))
        dump_alt_name("issuer alt name 2", ext);
    else if (!strcmp(ext->pszObjId, szOID_BASIC_CONSTRAINTS2))
        dump_basic_constraints2(ext);
    else if (!strcmp(ext->pszObjId, szOID_NAME_CONSTRAINTS))
        dump_name_constraints(ext);
    else if (!strcmp(ext->pszObjId, szOID_CERT_POLICIES))
        dump_cert_policies(ext);
    else if (!strcmp(ext->pszObjId, szOID_ENHANCED_KEY_USAGE))
        dump_enhanced_key_usage(ext);
    else if (!strcmp(ext->pszObjId, szOID_NETSCAPE_CERT_TYPE))
        dump_netscape_cert_type(ext);
}

static LPCWSTR filetime_to_str(const FILETIME *time)
{
    static WCHAR date[80];
    WCHAR dateFmt[80]; /* sufficient for all versions of LOCALE_SSHORTDATE */
    SYSTEMTIME sysTime;

    if (!time) return NULL;

    GetLocaleInfoW(LOCALE_SYSTEM_DEFAULT, LOCALE_SSHORTDATE, dateFmt,
     sizeof(dateFmt) / sizeof(dateFmt[0]));
    FileTimeToSystemTime(time, &sysTime);
    GetDateFormatW(LOCALE_SYSTEM_DEFAULT, 0, &sysTime, dateFmt, date,
     sizeof(date) / sizeof(date[0]));
    return date;
}

static void dump_element(PCCERT_CONTEXT cert)
{
    LPWSTR name = NULL;
    DWORD len, i;

    TRACE_(chain)("%p\n", cert);
    len = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
     CERT_NAME_ISSUER_FLAG, NULL, NULL, 0);
    name = CryptMemAlloc(len * sizeof(WCHAR));
    if (name)
    {
        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
         CERT_NAME_ISSUER_FLAG, NULL, name, len);
        TRACE_(chain)("issued by %s\n", debugstr_w(name));
        CryptMemFree(name);
    }
    len = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
     NULL, 0);
    name = CryptMemAlloc(len * sizeof(WCHAR));
    if (name)
    {
        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
         name, len);
        TRACE_(chain)("issued to %s\n", debugstr_w(name));
        CryptMemFree(name);
    }
    TRACE_(chain)("valid from %s to %s\n",
     debugstr_w(filetime_to_str(&cert->pCertInfo->NotBefore)),
     debugstr_w(filetime_to_str(&cert->pCertInfo->NotAfter)));
    TRACE_(chain)("%d extensions\n", cert->pCertInfo->cExtension);
    for (i = 0; i < cert->pCertInfo->cExtension; i++)
        dump_extension(&cert->pCertInfo->rgExtension[i]);
}

static BOOL CRYPT_KeyUsageValid(PCertificateChainEngine engine,
 PCCERT_CONTEXT cert, BOOL isRoot, BOOL isCA, DWORD index)
{
    PCERT_EXTENSION ext;
    BOOL ret;
    BYTE usageBits = 0;

    ext = CertFindExtension(szOID_KEY_USAGE, cert->pCertInfo->cExtension,
     cert->pCertInfo->rgExtension);
    if (ext)
    {
        CRYPT_BIT_BLOB usage;
        DWORD size = sizeof(usage);

        ret = CryptDecodeObjectEx(cert->dwCertEncodingType, X509_BITS,
         ext->Value.pbData, ext->Value.cbData, CRYPT_DECODE_NOCOPY_FLAG, NULL,
         &usage, &size);
        if (!ret)
            return FALSE;
        else if (usage.cbData > 2)
        {
            /* The key usage extension only defines 9 bits => no more than 2
             * bytes are needed to encode all known usages.
             */
            return FALSE;
        }
        else
        {
            /* The only bit relevant to chain validation is the keyCertSign
             * bit, which is always in the least significant byte of the
             * key usage bits.
             */
            usageBits = usage.pbData[usage.cbData - 1];
        }
    }
    if (isCA)
    {
        if (!ext)
        {
            /* MS appears to violate RFC 5280, section 4.2.1.3 (Key Usage)
             * here.  Quoting the RFC:
             * "This [key usage] extension MUST appear in certificates that
             * contain public keys that are used to validate digital signatures
             * on other public key certificates or CRLs."
             * MS appears to accept certs that do not contain key usage
             * extensions as CA certs.  V1 and V2 certificates did not have
             * extensions, and many root certificates are V1 certificates, so
             * perhaps this is prudent.  On the other hand, MS also accepts V3
             * certs without key usage extensions.  We are more restrictive:
             * we accept locally installed V1 or V2 certs as CA certs.
             * We also accept a lack of key usage extension on root certs,
             * which is implied in RFC 5280, section 6.1:  the trust anchor's
             * only requirement is that it was used to issue the next
             * certificate in the chain.
             */
            if (isRoot)
                ret = TRUE;
            else if (cert->pCertInfo->dwVersion == CERT_V1 ||
             cert->pCertInfo->dwVersion == CERT_V2)
            {
                PCCERT_CONTEXT localCert = CRYPT_FindCertInStore(
                 engine->hWorld, cert);

                ret = localCert != NULL;
                CertFreeCertificateContext(localCert);
            }
            else
                ret = FALSE;
            if (!ret)
                WARN_(chain)("no key usage extension on a CA cert\n");
        }
        else
        {
            if (!(usageBits & CERT_KEY_CERT_SIGN_KEY_USAGE))
            {
                WARN_(chain)("keyCertSign not asserted on a CA cert\n");
                ret = FALSE;
            }
            else
                ret = TRUE;
        }
    }
    else
    {
        if (ext && (usageBits & CERT_KEY_CERT_SIGN_KEY_USAGE))
        {
            WARN_(chain)("keyCertSign asserted on a non-CA cert\n");
            ret = FALSE;
        }
        else
            ret = TRUE;
    }
    return ret;
}

static BOOL CRYPT_ExtendedKeyUsageValidForCA(PCCERT_CONTEXT cert)
{
    PCERT_EXTENSION ext;
    BOOL ret;

    /* RFC 5280, section 4.2.1.12:  "In general, this extension will only
     * appear in end entity certificates."  And, "If a certificate contains
     * both a key usage extension and an extended key usage extension, then
     * both extensions MUST be processed independently and the certificate MUST
     * only be used for a purpose consistent with both extensions."  This seems
     * to imply that it should be checked if present, and ignored if not.
     * Unfortunately some CAs, e.g. the Thawte SGC CA, don't include the code
     * signing extended key usage, whereas they do include the keyCertSign
     * key usage.  Thus, when checking for a CA, we only require the
     * code signing extended key usage if the extended key usage is critical.
     */
    ext = CertFindExtension(szOID_ENHANCED_KEY_USAGE,
     cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);
    if (ext && ext->fCritical)
    {
        CERT_ENHKEY_USAGE *usage;
        DWORD size;

        ret = CryptDecodeObjectEx(cert->dwCertEncodingType,
         X509_ENHANCED_KEY_USAGE, ext->Value.pbData, ext->Value.cbData,
         CRYPT_DECODE_ALLOC_FLAG, NULL, &usage, &size);
        if (ret)
        {
            DWORD i;

            /* Explicitly require the code signing extended key usage for a CA
             * with an extended key usage extension.  That is, don't assume
             * a cert is allowed to be a CA if it specifies the
             * anyExtendedKeyUsage usage oid.  See again RFC 5280, section
             * 4.2.1.12: "Applications that require the presence of a
             * particular purpose MAY reject certificates that include the
             * anyExtendedKeyUsage OID but not the particular OID expected for
             * the application."
             */
            ret = FALSE;
            for (i = 0; !ret && i < usage->cUsageIdentifier; i++)
                if (!strcmp(usage->rgpszUsageIdentifier[i],
                 szOID_PKIX_KP_CODE_SIGNING))
                    ret = TRUE;
            LocalFree(usage);
        }
    }
    else
        ret = TRUE;
    return ret;
}

static BOOL CRYPT_CriticalExtensionsSupported(PCCERT_CONTEXT cert)
{
    BOOL ret = TRUE;
    DWORD i;

    for (i = 0; ret && i < cert->pCertInfo->cExtension; i++)
    {
        if (cert->pCertInfo->rgExtension[i].fCritical)
        {
            LPCSTR oid = cert->pCertInfo->rgExtension[i].pszObjId;

            if (!strcmp(oid, szOID_BASIC_CONSTRAINTS))
                ret = TRUE;
            else if (!strcmp(oid, szOID_BASIC_CONSTRAINTS2))
                ret = TRUE;
            else if (!strcmp(oid, szOID_NAME_CONSTRAINTS))
                ret = TRUE;
            else if (!strcmp(oid, szOID_KEY_USAGE))
                ret = TRUE;
            else if (!strcmp(oid, szOID_SUBJECT_ALT_NAME))
                ret = TRUE;
            else if (!strcmp(oid, szOID_SUBJECT_ALT_NAME2))
                ret = TRUE;
            else if (!strcmp(oid, szOID_ENHANCED_KEY_USAGE))
                ret = TRUE;
            else
            {
                FIXME("unsupported critical extension %s\n",
                 debugstr_a(oid));
                ret = FALSE;
            }
        }
    }
    return ret;
}

static BOOL CRYPT_IsCertVersionValid(PCCERT_CONTEXT cert)
{
    BOOL ret = TRUE;

    /* Checks whether the contents of the cert match the cert's version. */
    switch (cert->pCertInfo->dwVersion)
    {
    case CERT_V1:
        /* A V1 cert may not contain unique identifiers.  See RFC 5280,
         * section 4.1.2.8:
         * "These fields MUST only appear if the version is 2 or 3 (Section
         *  4.1.2.1).  These fields MUST NOT appear if the version is 1."
         */
        if (cert->pCertInfo->IssuerUniqueId.cbData ||
         cert->pCertInfo->SubjectUniqueId.cbData)
            ret = FALSE;
        /* A V1 cert may not contain extensions.  See RFC 5280, section 4.1.2.9:
         * "This field MUST only appear if the version is 3 (Section 4.1.2.1)."
         */
        if (cert->pCertInfo->cExtension)
            ret = FALSE;
        break;
    case CERT_V2:
        /* A V2 cert may not contain extensions.  See RFC 5280, section 4.1.2.9:
         * "This field MUST only appear if the version is 3 (Section 4.1.2.1)."
         */
        if (cert->pCertInfo->cExtension)
            ret = FALSE;
        break;
    case CERT_V3:
        /* Do nothing, all fields are allowed for V3 certs */
        break;
    default:
        WARN_(chain)("invalid cert version %d\n", cert->pCertInfo->dwVersion);
        ret = FALSE;
    }
    return ret;
}

static void CRYPT_CheckSimpleChain(PCertificateChainEngine engine,
 PCERT_SIMPLE_CHAIN chain, LPFILETIME time)
{
    PCERT_CHAIN_ELEMENT rootElement = chain->rgpElement[chain->cElement - 1];
    int i;
    BOOL pathLengthConstraintViolated = FALSE;
    CERT_BASIC_CONSTRAINTS2_INFO constraints = { FALSE, FALSE, 0 };

    TRACE_(chain)("checking chain with %d elements for time %s\n",
     chain->cElement, debugstr_w(filetime_to_str(time)));
    for (i = chain->cElement - 1; i >= 0; i--)
    {
        BOOL isRoot;

        if (TRACE_ON(chain))
            dump_element(chain->rgpElement[i]->pCertContext);
        if (i == chain->cElement - 1)
            isRoot = CRYPT_IsCertificateSelfSigned(
             chain->rgpElement[i]->pCertContext);
        else
            isRoot = FALSE;
        if (!CRYPT_IsCertVersionValid(chain->rgpElement[i]->pCertContext))
        {
            /* MS appears to accept certs whose versions don't match their
             * contents, so there isn't an appropriate error code.
             */
            chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
             CERT_TRUST_INVALID_EXTENSION;
        }
        if (CertVerifyTimeValidity(time,
         chain->rgpElement[i]->pCertContext->pCertInfo) != 0)
            chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
             CERT_TRUST_IS_NOT_TIME_VALID;
        if (i != 0)
        {
            /* Check the signature of the cert this issued */
            if (!CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING,
             CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT,
             (void *)chain->rgpElement[i - 1]->pCertContext,
             CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT,
             (void *)chain->rgpElement[i]->pCertContext, 0, NULL))
                chain->rgpElement[i - 1]->TrustStatus.dwErrorStatus |=
                 CERT_TRUST_IS_NOT_SIGNATURE_VALID;
            /* Once a path length constraint has been violated, every remaining
             * CA cert's basic constraints is considered invalid.
             */
            if (pathLengthConstraintViolated)
                chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
                 CERT_TRUST_INVALID_BASIC_CONSTRAINTS;
            else if (!CRYPT_CheckBasicConstraintsForCA(engine,
             chain->rgpElement[i]->pCertContext, &constraints, i - 1, isRoot,
             &pathLengthConstraintViolated))
                chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
                 CERT_TRUST_INVALID_BASIC_CONSTRAINTS;
            else if (constraints.fPathLenConstraint &&
             constraints.dwPathLenConstraint)
            {
                /* This one's valid - decrement max length */
                constraints.dwPathLenConstraint--;
            }
        }
        else
        {
            /* Check whether end cert has a basic constraints extension */
            if (!CRYPT_DecodeBasicConstraints(
             chain->rgpElement[i]->pCertContext, &constraints, FALSE))
                chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
                 CERT_TRUST_INVALID_BASIC_CONSTRAINTS;
        }
        if (!CRYPT_KeyUsageValid(engine, chain->rgpElement[i]->pCertContext,
         isRoot, constraints.fCA, i))
            chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
             CERT_TRUST_IS_NOT_VALID_FOR_USAGE;
        if (i != 0)
            if (!CRYPT_ExtendedKeyUsageValidForCA(
             chain->rgpElement[i]->pCertContext))
                chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
                 CERT_TRUST_IS_NOT_VALID_FOR_USAGE;
        if (CRYPT_IsSimpleChainCyclic(chain))
        {
            /* If the chain is cyclic, then the path length constraints
             * are violated, because the chain is infinitely long.
             */
            pathLengthConstraintViolated = TRUE;
            chain->TrustStatus.dwErrorStatus |=
             CERT_TRUST_IS_PARTIAL_CHAIN |
             CERT_TRUST_INVALID_BASIC_CONSTRAINTS;
        }
        /* Check whether every critical extension is supported */
        if (!CRYPT_CriticalExtensionsSupported(
         chain->rgpElement[i]->pCertContext))
            chain->rgpElement[i]->TrustStatus.dwErrorStatus |=
             CERT_TRUST_INVALID_EXTENSION;
        CRYPT_CombineTrustStatus(&chain->TrustStatus,
         &chain->rgpElement[i]->TrustStatus);
    }
    CRYPT_CheckChainNameConstraints(chain);
    if (CRYPT_IsCertificateSelfSigned(rootElement->pCertContext))
    {
        rootElement->TrustStatus.dwInfoStatus |=
         CERT_TRUST_IS_SELF_SIGNED | CERT_TRUST_HAS_NAME_MATCH_ISSUER;
        CRYPT_CheckRootCert(engine->hRoot, rootElement);
    }
    CRYPT_CombineTrustStatus(&chain->TrustStatus, &rootElement->TrustStatus);
}

static PCCERT_CONTEXT CRYPT_GetIssuer(HCERTSTORE store, PCCERT_CONTEXT subject,
 PCCERT_CONTEXT prevIssuer, DWORD *infoStatus)
{
    PCCERT_CONTEXT issuer = NULL;
    PCERT_EXTENSION ext;
    DWORD size;

    *infoStatus = 0;
    if ((ext = CertFindExtension(szOID_AUTHORITY_KEY_IDENTIFIER,
     subject->pCertInfo->cExtension, subject->pCertInfo->rgExtension)))
    {
        CERT_AUTHORITY_KEY_ID_INFO *info;
        BOOL ret;

        ret = CryptDecodeObjectEx(subject->dwCertEncodingType,
         X509_AUTHORITY_KEY_ID, ext->Value.pbData, ext->Value.cbData,
         CRYPT_DECODE_ALLOC_FLAG | CRYPT_DECODE_NOCOPY_FLAG, NULL,
         &info, &size);
        if (ret)
        {
            CERT_ID id;

            if (info->CertIssuer.cbData && info->CertSerialNumber.cbData)
            {
                id.dwIdChoice = CERT_ID_ISSUER_SERIAL_NUMBER;
                memcpy(&id.u.IssuerSerialNumber.Issuer, &info->CertIssuer,
                 sizeof(CERT_NAME_BLOB));
                memcpy(&id.u.IssuerSerialNumber.SerialNumber,
                 &info->CertSerialNumber, sizeof(CRYPT_INTEGER_BLOB));
                issuer = CertFindCertificateInStore(store,
                 subject->dwCertEncodingType, 0, CERT_FIND_CERT_ID, &id,
                 prevIssuer);
                if (issuer)
                    *infoStatus = CERT_TRUST_HAS_EXACT_MATCH_ISSUER;
            }
            else if (info->KeyId.cbData)
            {
                id.dwIdChoice = CERT_ID_KEY_IDENTIFIER;
                memcpy(&id.u.KeyId, &info->KeyId, sizeof(CRYPT_HASH_BLOB));
                issuer = CertFindCertificateInStore(store,
                 subject->dwCertEncodingType, 0, CERT_FIND_CERT_ID, &id,
                 prevIssuer);
                if (issuer)
                    *infoStatus = CERT_TRUST_HAS_KEY_MATCH_ISSUER;
            }
            LocalFree(info);
        }
    }
    else if ((ext = CertFindExtension(szOID_AUTHORITY_KEY_IDENTIFIER2,
     subject->pCertInfo->cExtension, subject->pCertInfo->rgExtension)))
    {
        CERT_AUTHORITY_KEY_ID2_INFO *info;
        BOOL ret;

        ret = CryptDecodeObjectEx(subject->dwCertEncodingType,
         X509_AUTHORITY_KEY_ID2, ext->Value.pbData, ext->Value.cbData,
         CRYPT_DECODE_ALLOC_FLAG | CRYPT_DECODE_NOCOPY_FLAG, NULL,
         &info, &size);
        if (ret)
        {
            CERT_ID id;

            if (info->AuthorityCertIssuer.cAltEntry &&
             info->AuthorityCertSerialNumber.cbData)
            {
                PCERT_ALT_NAME_ENTRY directoryName = NULL;
                DWORD i;

                for (i = 0; !directoryName &&
                 i < info->AuthorityCertIssuer.cAltEntry; i++)
                    if (info->AuthorityCertIssuer.rgAltEntry[i].dwAltNameChoice
                     == CERT_ALT_NAME_DIRECTORY_NAME)
                        directoryName =
                         &info->AuthorityCertIssuer.rgAltEntry[i];
                if (directoryName)
                {
                    id.dwIdChoice = CERT_ID_ISSUER_SERIAL_NUMBER;
                    memcpy(&id.u.IssuerSerialNumber.Issuer,
                     &directoryName->u.DirectoryName, sizeof(CERT_NAME_BLOB));
                    memcpy(&id.u.IssuerSerialNumber.SerialNumber,
                     &info->AuthorityCertSerialNumber,
                     sizeof(CRYPT_INTEGER_BLOB));
                    issuer = CertFindCertificateInStore(store,
                     subject->dwCertEncodingType, 0, CERT_FIND_CERT_ID, &id,
                     prevIssuer);
                    if (issuer)
                        *infoStatus = CERT_TRUST_HAS_EXACT_MATCH_ISSUER;
                }
                else
                    FIXME("no supported name type in authority key id2\n");
            }
            else if (info->KeyId.cbData)
            {
                id.dwIdChoice = CERT_ID_KEY_IDENTIFIER;
                memcpy(&id.u.KeyId, &info->KeyId, sizeof(CRYPT_HASH_BLOB));
                issuer = CertFindCertificateInStore(store,
                 subject->dwCertEncodingType, 0, CERT_FIND_CERT_ID, &id,
                 prevIssuer);
                if (issuer)
                    *infoStatus = CERT_TRUST_HAS_KEY_MATCH_ISSUER;
            }
            LocalFree(info);
        }
    }
    else
    {
        issuer = CertFindCertificateInStore(store,
         subject->dwCertEncodingType, 0, CERT_FIND_SUBJECT_NAME,
         &subject->pCertInfo->Issuer, prevIssuer);
        *infoStatus = CERT_TRUST_HAS_NAME_MATCH_ISSUER;
    }
    return issuer;
}

/* Builds a simple chain by finding an issuer for the last cert in the chain,
 * until reaching a self-signed cert, or until no issuer can be found.
 */
static BOOL CRYPT_BuildSimpleChain(const CertificateChainEngine *engine,
 HCERTSTORE world, PCERT_SIMPLE_CHAIN chain)
{
    BOOL ret = TRUE;
    PCCERT_CONTEXT cert = chain->rgpElement[chain->cElement - 1]->pCertContext;

    while (ret && !CRYPT_IsSimpleChainCyclic(chain) &&
     !CRYPT_IsCertificateSelfSigned(cert))
    {
        PCCERT_CONTEXT issuer = CRYPT_GetIssuer(world, cert, NULL,
         &chain->rgpElement[chain->cElement - 1]->TrustStatus.dwInfoStatus);

        if (issuer)
        {
            ret = CRYPT_AddCertToSimpleChain(engine, chain, issuer,
             chain->rgpElement[chain->cElement - 1]->TrustStatus.dwInfoStatus);
            /* CRYPT_AddCertToSimpleChain add-ref's the issuer, so free it to
             * close the enumeration that found it
             */
            CertFreeCertificateContext(issuer);
            cert = issuer;
        }
        else
        {
            TRACE_(chain)("Couldn't find issuer, halting chain creation\n");
            chain->TrustStatus.dwErrorStatus |= CERT_TRUST_IS_PARTIAL_CHAIN;
            break;
        }
    }
    return ret;
}

static BOOL CRYPT_GetSimpleChainForCert(PCertificateChainEngine engine,
 HCERTSTORE world, PCCERT_CONTEXT cert, LPFILETIME pTime,
 PCERT_SIMPLE_CHAIN *ppChain)
{
    BOOL ret = FALSE;
    PCERT_SIMPLE_CHAIN chain;

    TRACE("(%p, %p, %p, %p)\n", engine, world, cert, pTime);

    chain = CryptMemAlloc(sizeof(CERT_SIMPLE_CHAIN));
    if (chain)
    {
        memset(chain, 0, sizeof(CERT_SIMPLE_CHAIN));
        chain->cbSize = sizeof(CERT_SIMPLE_CHAIN);
        ret = CRYPT_AddCertToSimpleChain(engine, chain, cert, 0);
        if (ret)
        {
            ret = CRYPT_BuildSimpleChain(engine, world, chain);
            if (ret)
                CRYPT_CheckSimpleChain(engine, chain, pTime);
        }
        if (!ret)
        {
            CRYPT_FreeSimpleChain(chain);
            chain = NULL;
        }
        *ppChain = chain;
    }
    return ret;
}

static BOOL CRYPT_BuildCandidateChainFromCert(HCERTCHAINENGINE hChainEngine,
 PCCERT_CONTEXT cert, LPFILETIME pTime, HCERTSTORE hAdditionalStore,
 PCertificateChain *ppChain)
{
    PCertificateChainEngine engine = (PCertificateChainEngine)hChainEngine;
    PCERT_SIMPLE_CHAIN simpleChain = NULL;
    HCERTSTORE world;
    BOOL ret;

    world = CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0,
     CERT_STORE_CREATE_NEW_FLAG, NULL);
    CertAddStoreToCollection(world, engine->hWorld, 0, 0);
    if (hAdditionalStore)
        CertAddStoreToCollection(world, hAdditionalStore, 0, 0);
    /* FIXME: only simple chains are supported for now, as CTLs aren't
     * supported yet.
     */
    if ((ret = CRYPT_GetSimpleChainForCert(engine, world, cert, pTime,
     &simpleChain)))
    {
        PCertificateChain chain = CryptMemAlloc(sizeof(CertificateChain));

        if (chain)
        {
            chain->ref = 1;
            chain->world = world;
            chain->context.cbSize = sizeof(CERT_CHAIN_CONTEXT);
            chain->context.TrustStatus = simpleChain->TrustStatus;
            chain->context.cChain = 1;
            chain->context.rgpChain = CryptMemAlloc(sizeof(PCERT_SIMPLE_CHAIN));
            chain->context.rgpChain[0] = simpleChain;
            chain->context.cLowerQualityChainContext = 0;
            chain->context.rgpLowerQualityChainContext = NULL;
            chain->context.fHasRevocationFreshnessTime = FALSE;
            chain->context.dwRevocationFreshnessTime = 0;
        }
        else
            ret = FALSE;
        *ppChain = chain;
    }
    return ret;
}

/* Makes and returns a copy of chain, up to and including element iElement. */
static PCERT_SIMPLE_CHAIN CRYPT_CopySimpleChainToElement(
 const CERT_SIMPLE_CHAIN *chain, DWORD iElement)
{
    PCERT_SIMPLE_CHAIN copy = CryptMemAlloc(sizeof(CERT_SIMPLE_CHAIN));

    if (copy)
    {
        memset(copy, 0, sizeof(CERT_SIMPLE_CHAIN));
        copy->cbSize = sizeof(CERT_SIMPLE_CHAIN);
        copy->rgpElement =
         CryptMemAlloc((iElement + 1) * sizeof(PCERT_CHAIN_ELEMENT));
        if (copy->rgpElement)
        {
            DWORD i;
            BOOL ret = TRUE;

            memset(copy->rgpElement, 0,
             (iElement + 1) * sizeof(PCERT_CHAIN_ELEMENT));
            for (i = 0; ret && i <= iElement; i++)
            {
                PCERT_CHAIN_ELEMENT element =
                 CryptMemAlloc(sizeof(CERT_CHAIN_ELEMENT));

                if (element)
                {
                    *element = *chain->rgpElement[i];
                    element->pCertContext = CertDuplicateCertificateContext(
                     chain->rgpElement[i]->pCertContext);
                    /* Reset the trust status of the copied element, it'll get
                     * rechecked after the new chain is done.
                     */
                    memset(&element->TrustStatus, 0, sizeof(CERT_TRUST_STATUS));
                    copy->rgpElement[copy->cElement++] = element;
                }
                else
                    ret = FALSE;
            }
            if (!ret)
            {
                for (i = 0; i <= iElement; i++)
                    CryptMemFree(copy->rgpElement[i]);
                CryptMemFree(copy->rgpElement);
                CryptMemFree(copy);
                copy = NULL;
            }
        }
        else
        {
            CryptMemFree(copy);
            copy = NULL;
        }
    }
    return copy;
}

static void CRYPT_FreeLowerQualityChains(PCertificateChain chain)
{
    DWORD i;

    for (i = 0; i < chain->context.cLowerQualityChainContext; i++)
        CertFreeCertificateChain(chain->context.rgpLowerQualityChainContext[i]);
    CryptMemFree(chain->context.rgpLowerQualityChainContext);
    chain->context.cLowerQualityChainContext = 0;
    chain->context.rgpLowerQualityChainContext = NULL;
}

static void CRYPT_FreeChainContext(PCertificateChain chain)
{
    DWORD i;

    CRYPT_FreeLowerQualityChains(chain);
    for (i = 0; i < chain->context.cChain; i++)
        CRYPT_FreeSimpleChain(chain->context.rgpChain[i]);
    CryptMemFree(chain->context.rgpChain);
    CertCloseStore(chain->world, 0);
    CryptMemFree(chain);
}

/* Makes and returns a copy of chain, up to and including element iElement of
 * simple chain iChain.
 */
static PCertificateChain CRYPT_CopyChainToElement(PCertificateChain chain,
 DWORD iChain, DWORD iElement)
{
    PCertificateChain copy = CryptMemAlloc(sizeof(CertificateChain));

    if (copy)
    {
        copy->ref = 1;
        copy->world = CertDuplicateStore(chain->world);
        copy->context.cbSize = sizeof(CERT_CHAIN_CONTEXT);
        /* Leave the trust status of the copied chain unset, it'll get
         * rechecked after the new chain is done.
         */
        memset(&copy->context.TrustStatus, 0, sizeof(CERT_TRUST_STATUS));
        copy->context.cLowerQualityChainContext = 0;
        copy->context.rgpLowerQualityChainContext = NULL;
        copy->context.fHasRevocationFreshnessTime = FALSE;
        copy->context.dwRevocationFreshnessTime = 0;
        copy->context.rgpChain = CryptMemAlloc(
         (iChain + 1) * sizeof(PCERT_SIMPLE_CHAIN));
        if (copy->context.rgpChain)
        {
            BOOL ret = TRUE;
            DWORD i;

            memset(copy->context.rgpChain, 0,
             (iChain + 1) * sizeof(PCERT_SIMPLE_CHAIN));
            if (iChain)
            {
                for (i = 0; ret && iChain && i < iChain - 1; i++)
                {
                    copy->context.rgpChain[i] =
                     CRYPT_CopySimpleChainToElement(chain->context.rgpChain[i],
                     chain->context.rgpChain[i]->cElement - 1);
                    if (!copy->context.rgpChain[i])
                        ret = FALSE;
                }
            }
            else
                i = 0;
            if (ret)
            {
                copy->context.rgpChain[i] =
                 CRYPT_CopySimpleChainToElement(chain->context.rgpChain[i],
                 iElement);
                if (!copy->context.rgpChain[i])
                    ret = FALSE;
            }
            if (!ret)
            {
                CRYPT_FreeChainContext(copy);
                copy = NULL;
            }
            else
                copy->context.cChain = iChain + 1;
        }
        else
        {
            CryptMemFree(copy);
            copy = NULL;
        }
    }
    return copy;
}

static PCertificateChain CRYPT_BuildAlternateContextFromChain(
 HCERTCHAINENGINE hChainEngine, LPFILETIME pTime, HCERTSTORE hAdditionalStore,
 PCertificateChain chain)
{
    PCertificateChainEngine engine = (PCertificateChainEngine)hChainEngine;
    PCertificateChain alternate;

    TRACE("(%p, %p, %p, %p)\n", hChainEngine, pTime, hAdditionalStore, chain);

    /* Always start with the last "lower quality" chain to ensure a consistent
     * order of alternate creation:
     */
    if (chain->context.cLowerQualityChainContext)
        chain = (PCertificateChain)chain->context.rgpLowerQualityChainContext[
         chain->context.cLowerQualityChainContext - 1];
    /* A chain with only one element can't have any alternates */
    if (chain->context.cChain <= 1 && chain->context.rgpChain[0]->cElement <= 1)
        alternate = NULL;
    else
    {
        DWORD i, j, infoStatus;
        PCCERT_CONTEXT alternateIssuer = NULL;

        alternate = NULL;
        for (i = 0; !alternateIssuer && i < chain->context.cChain; i++)
            for (j = 0; !alternateIssuer &&
             j < chain->context.rgpChain[i]->cElement - 1; j++)
            {
                PCCERT_CONTEXT subject =
                 chain->context.rgpChain[i]->rgpElement[j]->pCertContext;
                PCCERT_CONTEXT prevIssuer = CertDuplicateCertificateContext(
                 chain->context.rgpChain[i]->rgpElement[j + 1]->pCertContext);

                alternateIssuer = CRYPT_GetIssuer(prevIssuer->hCertStore,
                 subject, prevIssuer, &infoStatus);
            }
        if (alternateIssuer)
        {
            i--;
            j--;
            alternate = CRYPT_CopyChainToElement(chain, i, j);
            if (alternate)
            {
                BOOL ret = CRYPT_AddCertToSimpleChain(engine,
                 alternate->context.rgpChain[i], alternateIssuer, infoStatus);

                /* CRYPT_AddCertToSimpleChain add-ref's the issuer, so free it
                 * to close the enumeration that found it
                 */
                CertFreeCertificateContext(alternateIssuer);
                if (ret)
                {
                    ret = CRYPT_BuildSimpleChain(engine, alternate->world,
                     alternate->context.rgpChain[i]);
                    if (ret)
                        CRYPT_CheckSimpleChain(engine,
                         alternate->context.rgpChain[i], pTime);
                    CRYPT_CombineTrustStatus(&alternate->context.TrustStatus,
                     &alternate->context.rgpChain[i]->TrustStatus);
                }
                if (!ret)
                {
                    CRYPT_FreeChainContext(alternate);
                    alternate = NULL;
                }
            }
        }
    }
    TRACE("%p\n", alternate);
    return alternate;
}

#define CHAIN_QUALITY_SIGNATURE_VALID   0x16
#define CHAIN_QUALITY_TIME_VALID        8
#define CHAIN_QUALITY_COMPLETE_CHAIN    4
#define CHAIN_QUALITY_BASIC_CONSTRAINTS 2
#define CHAIN_QUALITY_TRUSTED_ROOT      1

#define CHAIN_QUALITY_HIGHEST \
 CHAIN_QUALITY_SIGNATURE_VALID | CHAIN_QUALITY_TIME_VALID | \
 CHAIN_QUALITY_COMPLETE_CHAIN | CHAIN_QUALITY_BASIC_CONSTRAINTS | \
 CHAIN_QUALITY_TRUSTED_ROOT

#define IS_TRUST_ERROR_SET(TrustStatus, bits) \
 (TrustStatus)->dwErrorStatus & (bits)

static DWORD CRYPT_ChainQuality(const CertificateChain *chain)
{
    DWORD quality = CHAIN_QUALITY_HIGHEST;

    if (IS_TRUST_ERROR_SET(&chain->context.TrustStatus,
     CERT_TRUST_IS_UNTRUSTED_ROOT))
        quality &= ~CHAIN_QUALITY_TRUSTED_ROOT;
    if (IS_TRUST_ERROR_SET(&chain->context.TrustStatus,
     CERT_TRUST_INVALID_BASIC_CONSTRAINTS))
        quality &= ~CHAIN_QUALITY_BASIC_CONSTRAINTS;
    if (IS_TRUST_ERROR_SET(&chain->context.TrustStatus,
     CERT_TRUST_IS_PARTIAL_CHAIN))
        quality &= ~CHAIN_QUALITY_COMPLETE_CHAIN;
    if (IS_TRUST_ERROR_SET(&chain->context.TrustStatus,
     CERT_TRUST_IS_NOT_TIME_VALID | CERT_TRUST_IS_NOT_TIME_NESTED))
        quality &= ~CHAIN_QUALITY_TIME_VALID;
    if (IS_TRUST_ERROR_SET(&chain->context.TrustStatus,
     CERT_TRUST_IS_NOT_SIGNATURE_VALID))
        quality &= ~CHAIN_QUALITY_SIGNATURE_VALID;
    return quality;
}

/* Chooses the highest quality chain among chain and its "lower quality"
 * alternate chains.  Returns the highest quality chain, with all other
 * chains as lower quality chains of it.
 */
static PCertificateChain CRYPT_ChooseHighestQualityChain(
 PCertificateChain chain)
{
    DWORD i;

    /* There are always only two chains being considered:  chain, and an
     * alternate at chain->rgpLowerQualityChainContext[i].  If the alternate
     * has a higher quality than chain, the alternate gets assigned the lower
     * quality contexts, with chain taking the alternate's place among the
     * lower quality contexts.
     */
    for (i = 0; i < chain->context.cLowerQualityChainContext; i++)
    {
        PCertificateChain alternate =
         (PCertificateChain)chain->context.rgpLowerQualityChainContext[i];

        if (CRYPT_ChainQuality(alternate) > CRYPT_ChainQuality(chain))
        {
            alternate->context.cLowerQualityChainContext =
             chain->context.cLowerQualityChainContext;
            alternate->context.rgpLowerQualityChainContext =
             chain->context.rgpLowerQualityChainContext;
            alternate->context.rgpLowerQualityChainContext[i] =
             (PCCERT_CHAIN_CONTEXT)chain;
            chain->context.cLowerQualityChainContext = 0;
            chain->context.rgpLowerQualityChainContext = NULL;
            chain = alternate;
        }
    }
    return chain;
}

static BOOL CRYPT_AddAlternateChainToChain(PCertificateChain chain,
 const CertificateChain *alternate)
{
    BOOL ret;

    if (chain->context.cLowerQualityChainContext)
        chain->context.rgpLowerQualityChainContext =
         CryptMemRealloc(chain->context.rgpLowerQualityChainContext,
         (chain->context.cLowerQualityChainContext + 1) *
         sizeof(PCCERT_CHAIN_CONTEXT));
    else
        chain->context.rgpLowerQualityChainContext =
         CryptMemAlloc(sizeof(PCCERT_CHAIN_CONTEXT));
    if (chain->context.rgpLowerQualityChainContext)
    {
        chain->context.rgpLowerQualityChainContext[
         chain->context.cLowerQualityChainContext++] =
         (PCCERT_CHAIN_CONTEXT)alternate;
        ret = TRUE;
    }
    else
        ret = FALSE;
    return ret;
}

static PCERT_CHAIN_ELEMENT CRYPT_FindIthElementInChain(
 const CERT_CHAIN_CONTEXT *chain, DWORD i)
{
    DWORD j, iElement;
    PCERT_CHAIN_ELEMENT element = NULL;

    for (j = 0, iElement = 0; !element && j < chain->cChain; j++)
    {
        if (iElement + chain->rgpChain[j]->cElement < i)
            iElement += chain->rgpChain[j]->cElement;
        else
            element = chain->rgpChain[j]->rgpElement[i - iElement];
    }
    return element;
}

typedef struct _CERT_CHAIN_PARA_NO_EXTRA_FIELDS {
    DWORD            cbSize;
    CERT_USAGE_MATCH RequestedUsage;
} CERT_CHAIN_PARA_NO_EXTRA_FIELDS, *PCERT_CHAIN_PARA_NO_EXTRA_FIELDS;

static void CRYPT_VerifyChainRevocation(PCERT_CHAIN_CONTEXT chain,
 LPFILETIME pTime, const CERT_CHAIN_PARA *pChainPara, DWORD chainFlags)
{
    DWORD cContext;

    if (chainFlags & CERT_CHAIN_REVOCATION_CHECK_END_CERT)
        cContext = 1;
    else if ((chainFlags & CERT_CHAIN_REVOCATION_CHECK_CHAIN) ||
     (chainFlags & CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT))
    {
        DWORD i;

        for (i = 0, cContext = 0; i < chain->cChain; i++)
        {
            if (i < chain->cChain - 1 ||
             chainFlags & CERT_CHAIN_REVOCATION_CHECK_CHAIN)
                cContext += chain->rgpChain[i]->cElement;
            else
                cContext += chain->rgpChain[i]->cElement - 1;
        }
    }
    else
        cContext = 0;
    if (cContext)
    {
        PCCERT_CONTEXT *contexts =
         CryptMemAlloc(cContext * sizeof(PCCERT_CONTEXT *));

        if (contexts)
        {
            DWORD i, j, iContext, revocationFlags;
            CERT_REVOCATION_PARA revocationPara = { sizeof(revocationPara), 0 };
            CERT_REVOCATION_STATUS revocationStatus =
             { sizeof(revocationStatus), 0 };
            BOOL ret;

            for (i = 0, iContext = 0; iContext < cContext && i < chain->cChain;
             i++)
            {
                for (j = 0; iContext < cContext &&
                 j < chain->rgpChain[i]->cElement; j++)
                    contexts[iContext++] =
                     chain->rgpChain[i]->rgpElement[j]->pCertContext;
            }
            revocationFlags = CERT_VERIFY_REV_CHAIN_FLAG;
            if (chainFlags & CERT_CHAIN_REVOCATION_CHECK_CACHE_ONLY)
                revocationFlags |= CERT_VERIFY_CACHE_ONLY_BASED_REVOCATION;
            if (chainFlags & CERT_CHAIN_REVOCATION_ACCUMULATIVE_TIMEOUT)
                revocationFlags |= CERT_VERIFY_REV_ACCUMULATIVE_TIMEOUT_FLAG;
            revocationPara.pftTimeToUse = pTime;
            if (pChainPara->cbSize == sizeof(CERT_CHAIN_PARA))
            {
                revocationPara.dwUrlRetrievalTimeout =
                 pChainPara->dwUrlRetrievalTimeout;
                revocationPara.fCheckFreshnessTime =
                 pChainPara->fCheckRevocationFreshnessTime;
                revocationPara.dwFreshnessTime =
                 pChainPara->dwRevocationFreshnessTime;
            }
            ret = CertVerifyRevocation(X509_ASN_ENCODING,
             CERT_CONTEXT_REVOCATION_TYPE, cContext, (void **)contexts,
             revocationFlags, &revocationPara, &revocationStatus);
            if (!ret)
            {
                PCERT_CHAIN_ELEMENT element =
                 CRYPT_FindIthElementInChain(chain, revocationStatus.dwIndex);
                DWORD error;

                switch (revocationStatus.dwError)
                {
                case CRYPT_E_NO_REVOCATION_CHECK:
                case CRYPT_E_NO_REVOCATION_DLL:
                case CRYPT_E_NOT_IN_REVOCATION_DATABASE:
                    error = CERT_TRUST_REVOCATION_STATUS_UNKNOWN;
                    break;
                case CRYPT_E_REVOCATION_OFFLINE:
                    error = CERT_TRUST_IS_OFFLINE_REVOCATION;
                    break;
                case CRYPT_E_REVOKED:
                    error = CERT_TRUST_IS_REVOKED;
                    break;
                default:
                    WARN("unmapped error %08x\n", revocationStatus.dwError);
                    error = 0;
                }
                if (element)
                {
                    /* FIXME: set element's pRevocationInfo member */
                    element->TrustStatus.dwErrorStatus |= error;
                }
                chain->TrustStatus.dwErrorStatus |= error;
            }
            CryptMemFree(contexts);
        }
    }
}

static void dump_usage_match(LPCSTR name, const CERT_USAGE_MATCH *usageMatch)
{
    DWORD i;

    TRACE_(chain)("%s: %s\n", name,
     usageMatch->dwType == USAGE_MATCH_TYPE_AND ? "AND" : "OR");
    for (i = 0; i < usageMatch->Usage.cUsageIdentifier; i++)
        TRACE_(chain)("%s\n", usageMatch->Usage.rgpszUsageIdentifier[i]);
}

static void dump_chain_para(const CERT_CHAIN_PARA *pChainPara)
{
    TRACE_(chain)("%d\n", pChainPara->cbSize);
    if (pChainPara->cbSize >= sizeof(CERT_CHAIN_PARA_NO_EXTRA_FIELDS))
        dump_usage_match("RequestedUsage", &pChainPara->RequestedUsage);
    if (pChainPara->cbSize >= sizeof(CERT_CHAIN_PARA))
    {
        dump_usage_match("RequestedIssuancePolicy",
         &pChainPara->RequestedIssuancePolicy);
        TRACE_(chain)("%d\n", pChainPara->dwUrlRetrievalTimeout);
        TRACE_(chain)("%d\n", pChainPara->fCheckRevocationFreshnessTime);
        TRACE_(chain)("%d\n", pChainPara->dwRevocationFreshnessTime);
    }
}

BOOL WINAPI CertGetCertificateChain(HCERTCHAINENGINE hChainEngine,
 PCCERT_CONTEXT pCertContext, LPFILETIME pTime, HCERTSTORE hAdditionalStore,
 PCERT_CHAIN_PARA pChainPara, DWORD dwFlags, LPVOID pvReserved,
 PCCERT_CHAIN_CONTEXT* ppChainContext)
{
    BOOL ret;
    PCertificateChain chain = NULL;

    TRACE("(%p, %p, %p, %p, %p, %08x, %p, %p)\n", hChainEngine, pCertContext,
     pTime, hAdditionalStore, pChainPara, dwFlags, pvReserved, ppChainContext);

    if (ppChainContext)
        *ppChainContext = NULL;
    if (!pChainPara)
    {
        SetLastError(E_INVALIDARG);
        return FALSE;
    }
    if (!pCertContext->pCertInfo->SignatureAlgorithm.pszObjId)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    if (!hChainEngine)
        hChainEngine = CRYPT_GetDefaultChainEngine();
    if (TRACE_ON(chain))
        dump_chain_para(pChainPara);
    /* FIXME: what about HCCE_LOCAL_MACHINE? */
    ret = CRYPT_BuildCandidateChainFromCert(hChainEngine, pCertContext, pTime,
     hAdditionalStore, &chain);
    if (ret)
    {
        PCertificateChain alternate = NULL;
        PCERT_CHAIN_CONTEXT pChain;

        do {
            alternate = CRYPT_BuildAlternateContextFromChain(hChainEngine,
             pTime, hAdditionalStore, chain);

            /* Alternate contexts are added as "lower quality" contexts of
             * chain, to avoid loops in alternate chain creation.
             * The highest-quality chain is chosen at the end.
             */
            if (alternate)
                ret = CRYPT_AddAlternateChainToChain(chain, alternate);
        } while (ret && alternate);
        chain = CRYPT_ChooseHighestQualityChain(chain);
        if (!(dwFlags & CERT_CHAIN_RETURN_LOWER_QUALITY_CONTEXTS))
            CRYPT_FreeLowerQualityChains(chain);
        pChain = (PCERT_CHAIN_CONTEXT)chain;
        CRYPT_VerifyChainRevocation(pChain, pTime, pChainPara, dwFlags);
        if (ppChainContext)
            *ppChainContext = pChain;
        else
            CertFreeCertificateChain(pChain);
    }
    TRACE("returning %d\n", ret);
    return ret;
}

PCCERT_CHAIN_CONTEXT WINAPI CertDuplicateCertificateChain(
 PCCERT_CHAIN_CONTEXT pChainContext)
{
    PCertificateChain chain = (PCertificateChain)pChainContext;

    TRACE("(%p)\n", pChainContext);

    if (chain)
        InterlockedIncrement(&chain->ref);
    return pChainContext;
}

VOID WINAPI CertFreeCertificateChain(PCCERT_CHAIN_CONTEXT pChainContext)
{
    PCertificateChain chain = (PCertificateChain)pChainContext;

    TRACE("(%p)\n", pChainContext);

    if (chain)
    {
        if (InterlockedDecrement(&chain->ref) == 0)
            CRYPT_FreeChainContext(chain);
    }
}

static void find_element_with_error(PCCERT_CHAIN_CONTEXT chain, DWORD error,
 LONG *iChain, LONG *iElement)
{
    DWORD i, j;

    for (i = 0; i < chain->cChain; i++)
        for (j = 0; j < chain->rgpChain[i]->cElement; j++)
            if (chain->rgpChain[i]->rgpElement[j]->TrustStatus.dwErrorStatus &
             error)
            {
                *iChain = i;
                *iElement = j;
                return;
            }
}

static BOOL WINAPI verify_base_policy(LPCSTR szPolicyOID,
 PCCERT_CHAIN_CONTEXT pChainContext, PCERT_CHAIN_POLICY_PARA pPolicyPara,
 PCERT_CHAIN_POLICY_STATUS pPolicyStatus)
{
    pPolicyStatus->lChainIndex = pPolicyStatus->lElementIndex = -1;
    if (pChainContext->TrustStatus.dwErrorStatus &
     CERT_TRUST_IS_NOT_SIGNATURE_VALID)
    {
        pPolicyStatus->dwError = TRUST_E_CERT_SIGNATURE;
        find_element_with_error(pChainContext,
         CERT_TRUST_IS_NOT_SIGNATURE_VALID, &pPolicyStatus->lChainIndex,
         &pPolicyStatus->lElementIndex);
    }
    else if (pChainContext->TrustStatus.dwErrorStatus &
     CERT_TRUST_IS_UNTRUSTED_ROOT)
    {
        pPolicyStatus->dwError = CERT_E_UNTRUSTEDROOT;
        find_element_with_error(pChainContext,
         CERT_TRUST_IS_UNTRUSTED_ROOT, &pPolicyStatus->lChainIndex,
         &pPolicyStatus->lElementIndex);
    }
    else if (pChainContext->TrustStatus.dwErrorStatus & CERT_TRUST_IS_CYCLIC)
    {
        pPolicyStatus->dwError = CERT_E_CHAINING;
        find_element_with_error(pChainContext, CERT_TRUST_IS_CYCLIC,
         &pPolicyStatus->lChainIndex, &pPolicyStatus->lElementIndex);
        /* For a cyclic chain, which element is a cycle isn't meaningful */
        pPolicyStatus->lElementIndex = -1;
    }
    else
        pPolicyStatus->dwError = NO_ERROR;
    return TRUE;
}

static BYTE msTestPubKey1[] = {
0x30,0x47,0x02,0x40,0x81,0x55,0x22,0xb9,0x8a,0xa4,0x6f,0xed,0xd6,0xe7,0xd9,
0x66,0x0f,0x55,0xbc,0xd7,0xcd,0xd5,0xbc,0x4e,0x40,0x02,0x21,0xa2,0xb1,0xf7,
0x87,0x30,0x85,0x5e,0xd2,0xf2,0x44,0xb9,0xdc,0x9b,0x75,0xb6,0xfb,0x46,0x5f,
0x42,0xb6,0x9d,0x23,0x36,0x0b,0xde,0x54,0x0f,0xcd,0xbd,0x1f,0x99,0x2a,0x10,
0x58,0x11,0xcb,0x40,0xcb,0xb5,0xa7,0x41,0x02,0x03,0x01,0x00,0x01 };
static BYTE msTestPubKey2[] = {
0x30,0x47,0x02,0x40,0x9c,0x50,0x05,0x1d,0xe2,0x0e,0x4c,0x53,0xd8,0xd9,0xb5,
0xe5,0xfd,0xe9,0xe3,0xad,0x83,0x4b,0x80,0x08,0xd9,0xdc,0xe8,0xe8,0x35,0xf8,
0x11,0xf1,0xe9,0x9b,0x03,0x7a,0x65,0x64,0x76,0x35,0xce,0x38,0x2c,0xf2,0xb6,
0x71,0x9e,0x06,0xd9,0xbf,0xbb,0x31,0x69,0xa3,0xf6,0x30,0xa0,0x78,0x7b,0x18,
0xdd,0x50,0x4d,0x79,0x1e,0xeb,0x61,0xc1,0x02,0x03,0x01,0x00,0x01 };

static BOOL WINAPI verify_authenticode_policy(LPCSTR szPolicyOID,
 PCCERT_CHAIN_CONTEXT pChainContext, PCERT_CHAIN_POLICY_PARA pPolicyPara,
 PCERT_CHAIN_POLICY_STATUS pPolicyStatus)
{
    BOOL ret = verify_base_policy(szPolicyOID, pChainContext, pPolicyPara,
     pPolicyStatus);

    if (ret && pPolicyStatus->dwError == CERT_E_UNTRUSTEDROOT)
    {
        CERT_PUBLIC_KEY_INFO msPubKey = { { 0 } };
        BOOL isMSTestRoot = FALSE;
        PCCERT_CONTEXT failingCert =
         pChainContext->rgpChain[pPolicyStatus->lChainIndex]->
         rgpElement[pPolicyStatus->lElementIndex]->pCertContext;
        DWORD i;
        CRYPT_DATA_BLOB keyBlobs[] = {
         { sizeof(msTestPubKey1), msTestPubKey1 },
         { sizeof(msTestPubKey2), msTestPubKey2 },
        };

        /* Check whether the root is an MS test root */
        for (i = 0; !isMSTestRoot && i < sizeof(keyBlobs) / sizeof(keyBlobs[0]);
         i++)
        {
            msPubKey.PublicKey.cbData = keyBlobs[i].cbData;
            msPubKey.PublicKey.pbData = keyBlobs[i].pbData;
            if (CertComparePublicKeyInfo(
             X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
             &failingCert->pCertInfo->SubjectPublicKeyInfo, &msPubKey))
                isMSTestRoot = TRUE;
        }
        if (isMSTestRoot)
            pPolicyStatus->dwError = CERT_E_UNTRUSTEDTESTROOT;
    }
    return ret;
}

static BOOL WINAPI verify_basic_constraints_policy(LPCSTR szPolicyOID,
 PCCERT_CHAIN_CONTEXT pChainContext, PCERT_CHAIN_POLICY_PARA pPolicyPara,
 PCERT_CHAIN_POLICY_STATUS pPolicyStatus)
{
    pPolicyStatus->lChainIndex = pPolicyStatus->lElementIndex = -1;
    if (pChainContext->TrustStatus.dwErrorStatus &
     CERT_TRUST_INVALID_BASIC_CONSTRAINTS)
    {
        pPolicyStatus->dwError = TRUST_E_BASIC_CONSTRAINTS;
        find_element_with_error(pChainContext,
         CERT_TRUST_INVALID_BASIC_CONSTRAINTS, &pPolicyStatus->lChainIndex,
         &pPolicyStatus->lElementIndex);
    }
    else
        pPolicyStatus->dwError = NO_ERROR;
    return TRUE;
}

static inline PCERT_EXTENSION get_subject_alt_name_ext(PCCERT_CONTEXT cert)
{
    PCERT_EXTENSION ext;

    ext = CertFindExtension(szOID_SUBJECT_ALT_NAME2,
     cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);
    if (!ext)
        ext = CertFindExtension(szOID_SUBJECT_ALT_NAME,
         cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);
    return ext;
}

static BOOL match_dns_to_subject_alt_name(PCERT_EXTENSION ext,
 LPCWSTR server_name)
{
    BOOL matches = FALSE;
    CERT_ALT_NAME_INFO *subjectName;
    DWORD size;

    TRACE_(chain)("%s\n", debugstr_w(server_name));
    /* FIXME: This can be spoofed by the embedded NULL vulnerability.  The
     * returned CERT_ALT_NAME_INFO doesn't have a way to indicate the
     * encoded length of a name, so a certificate issued to
     * winehq.org\0badsite.com will get treated as having been issued to
     * winehq.org.
     */
    if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME,
     ext->Value.pbData, ext->Value.cbData,
     CRYPT_DECODE_ALLOC_FLAG | CRYPT_DECODE_NOCOPY_FLAG, NULL,
     &subjectName, &size))
    {
        DWORD i;
        BOOL found = FALSE;

        for (i = 0; !found && i < subjectName->cAltEntry; i++)
        {
            if (subjectName->rgAltEntry[i].dwAltNameChoice ==
             CERT_ALT_NAME_DNS_NAME)
            {
                TRACE_(chain)("dNSName: %s\n", debugstr_w(
                 subjectName->rgAltEntry[i].u.pwszDNSName));
                found = TRUE;
                if (!strcmpiW(server_name,
                 subjectName->rgAltEntry[i].u.pwszDNSName))
                    matches = TRUE;
            }
        }
        LocalFree(subjectName);
    }
    return matches;
}

static BOOL find_matching_domain_component(CERT_NAME_INFO *name,
 LPCWSTR component)
{
    BOOL matches = FALSE;
    DWORD i, j;

    for (i = 0; !matches && i < name->cRDN; i++)
        for (j = 0; j < name->rgRDN[i].cRDNAttr; j++)
            if (!strcmp(szOID_DOMAIN_COMPONENT,
             name->rgRDN[i].rgRDNAttr[j].pszObjId))
            {
                PCERT_RDN_ATTR attr;

                attr = &name->rgRDN[i].rgRDNAttr[j];
                /* Compare with memicmpW rather than strcmpiW in order to avoid
                 * a match with a string with an embedded NULL.  The component
                 * must match one domain component attribute's entire string
                 * value with a case-insensitive match.
                 */
                matches = !memicmpW(component, (LPWSTR)attr->Value.pbData,
                 attr->Value.cbData / sizeof(WCHAR));
            }
    return matches;
}

static BOOL match_dns_to_subject_dn(PCCERT_CONTEXT cert, LPCWSTR server_name)
{
    BOOL matches = FALSE;
    CERT_NAME_INFO *name;
    DWORD size;

    TRACE_(chain)("%s\n", debugstr_w(server_name));
    if (CryptDecodeObjectEx(X509_ASN_ENCODING, X509_UNICODE_NAME,
     cert->pCertInfo->Subject.pbData, cert->pCertInfo->Subject.cbData,
     CRYPT_DECODE_ALLOC_FLAG | CRYPT_DECODE_NOCOPY_FLAG, NULL,
     &name, &size))
    {
        /* If the subject distinguished name contains any name components,
         * make sure all of them are present.
         */
        if (CertFindRDNAttr(szOID_DOMAIN_COMPONENT, name))
        {
            LPCWSTR ptr = server_name;

            matches = TRUE;
            do {
                LPCWSTR dot = strchrW(ptr, '.'), end;
                /* 254 is the maximum DNS label length, see RFC 1035 */
                WCHAR component[255];
                DWORD len;

                end = dot ? dot : ptr + strlenW(ptr);
                len = end - ptr;
                if (len >= sizeof(component) / sizeof(component[0]))
                {
                    WARN_(chain)("domain component %s too long\n",
                     debugstr_wn(ptr, len));
                    matches = FALSE;
                }
                else
                {
                    memcpy(component, ptr, len * sizeof(WCHAR));
                    component[len] = 0;
                    matches = find_matching_domain_component(name, component);
                }
                ptr = dot ? dot + 1 : end;
            } while (matches && ptr && *ptr);
        }
        else
        {
            PCERT_RDN_ATTR attr;

            /* If the certificate isn't using a DN attribute in the name, make
             * make sure the common name matches.  Again, use memicmpW rather
             * than strcmpiW in order to avoid being fooled by an embedded NULL.
             */
            if ((attr = CertFindRDNAttr(szOID_COMMON_NAME, name)))
            {
                TRACE_(chain)("CN = %s\n", debugstr_w(
                 (LPWSTR)attr->Value.pbData));
                matches = !memicmpW(server_name, (LPWSTR)attr->Value.pbData,
                 attr->Value.cbData / sizeof(WCHAR));
            }
        }
        LocalFree(name);
    }
    return matches;
}

static BOOL WINAPI verify_ssl_policy(LPCSTR szPolicyOID,
 PCCERT_CHAIN_CONTEXT pChainContext, PCERT_CHAIN_POLICY_PARA pPolicyPara,
 PCERT_CHAIN_POLICY_STATUS pPolicyStatus)
{
    pPolicyStatus->lChainIndex = pPolicyStatus->lElementIndex = -1;
    if (pChainContext->TrustStatus.dwErrorStatus &
     CERT_TRUST_IS_NOT_SIGNATURE_VALID)
    {
        pPolicyStatus->dwError = TRUST_E_CERT_SIGNATURE;
        find_element_with_error(pChainContext,
         CERT_TRUST_IS_NOT_SIGNATURE_VALID, &pPolicyStatus->lChainIndex,
         &pPolicyStatus->lElementIndex);
    }
    else if (pChainContext->TrustStatus.dwErrorStatus &
     CERT_TRUST_IS_UNTRUSTED_ROOT)
    {
        pPolicyStatus->dwError = CERT_E_UNTRUSTEDROOT;
        find_element_with_error(pChainContext,
         CERT_TRUST_IS_UNTRUSTED_ROOT, &pPolicyStatus->lChainIndex,
         &pPolicyStatus->lElementIndex);
    }
    else if (pChainContext->TrustStatus.dwErrorStatus & CERT_TRUST_IS_CYCLIC)
    {
        pPolicyStatus->dwError = CERT_E_UNTRUSTEDROOT;
        find_element_with_error(pChainContext,
         CERT_TRUST_IS_CYCLIC, &pPolicyStatus->lChainIndex,
         &pPolicyStatus->lElementIndex);
        /* For a cyclic chain, which element is a cycle isn't meaningful */
        pPolicyStatus->lElementIndex = -1;
    }
    else if (pChainContext->TrustStatus.dwErrorStatus &
     CERT_TRUST_IS_NOT_TIME_VALID)
    {
        pPolicyStatus->dwError = CERT_E_EXPIRED;
        find_element_with_error(pChainContext,
         CERT_TRUST_IS_NOT_TIME_VALID, &pPolicyStatus->lChainIndex,
         &pPolicyStatus->lElementIndex);
    }
    else
        pPolicyStatus->dwError = NO_ERROR;
    /* We only need bother checking whether the name in the end certificate
     * matches if the chain is otherwise okay.
     */
    if (!pPolicyStatus->dwError && pPolicyPara &&
     pPolicyPara->cbSize >= sizeof(CERT_CHAIN_POLICY_PARA))
    {
        HTTPSPolicyCallbackData *sslPara = pPolicyPara->pvExtraPolicyPara;

        if (sslPara && sslPara->u.cbSize >= sizeof(HTTPSPolicyCallbackData))
        {
            if (sslPara->dwAuthType == AUTHTYPE_SERVER &&
             sslPara->pwszServerName)
            {
                PCCERT_CONTEXT cert;
                PCERT_EXTENSION altNameExt;
                BOOL matches;

                cert = pChainContext->rgpChain[0]->rgpElement[0]->pCertContext;
                altNameExt = get_subject_alt_name_ext(cert);
                /* If the alternate name extension exists, the name it contains
                 * is bound to the certificate, so make sure the name matches
                 * it.  Otherwise, look for the server name in the subject
                 * distinguished name.  RFC5280, section 4.2.1.6:
                 * "Whenever such identities are to be bound into a
                 *  certificate, the subject alternative name (or issuer
                 *  alternative name) extension MUST be used; however, a DNS
                 *  name MAY also be represented in the subject field using the
                 *  domainComponent attribute."
                 */
                if (altNameExt)
                    matches = match_dns_to_subject_alt_name(altNameExt,
                     sslPara->pwszServerName);
                else
                    matches = match_dns_to_subject_dn(cert,
                     sslPara->pwszServerName);
                if (!matches)
                {
                    pPolicyStatus->dwError = CERT_E_CN_NO_MATCH;
                    pPolicyStatus->lChainIndex = 0;
                    pPolicyStatus->lElementIndex = 0;
                }
            }
        }
    }
    return TRUE;
}

static BYTE msPubKey1[] = {
0x30,0x82,0x01,0x0a,0x02,0x82,0x01,0x01,0x00,0xdf,0x08,0xba,0xe3,0x3f,0x6e,
0x64,0x9b,0xf5,0x89,0xaf,0x28,0x96,0x4a,0x07,0x8f,0x1b,0x2e,0x8b,0x3e,0x1d,
0xfc,0xb8,0x80,0x69,0xa3,0xa1,0xce,0xdb,0xdf,0xb0,0x8e,0x6c,0x89,0x76,0x29,
0x4f,0xca,0x60,0x35,0x39,0xad,0x72,0x32,0xe0,0x0b,0xae,0x29,0x3d,0x4c,0x16,
0xd9,0x4b,0x3c,0x9d,0xda,0xc5,0xd3,0xd1,0x09,0xc9,0x2c,0x6f,0xa6,0xc2,0x60,
0x53,0x45,0xdd,0x4b,0xd1,0x55,0xcd,0x03,0x1c,0xd2,0x59,0x56,0x24,0xf3,0xe5,
0x78,0xd8,0x07,0xcc,0xd8,0xb3,0x1f,0x90,0x3f,0xc0,0x1a,0x71,0x50,0x1d,0x2d,
0xa7,0x12,0x08,0x6d,0x7c,0xb0,0x86,0x6c,0xc7,0xba,0x85,0x32,0x07,0xe1,0x61,
0x6f,0xaf,0x03,0xc5,0x6d,0xe5,0xd6,0xa1,0x8f,0x36,0xf6,0xc1,0x0b,0xd1,0x3e,
0x69,0x97,0x48,0x72,0xc9,0x7f,0xa4,0xc8,0xc2,0x4a,0x4c,0x7e,0xa1,0xd1,0x94,
0xa6,0xd7,0xdc,0xeb,0x05,0x46,0x2e,0xb8,0x18,0xb4,0x57,0x1d,0x86,0x49,0xdb,
0x69,0x4a,0x2c,0x21,0xf5,0x5e,0x0f,0x54,0x2d,0x5a,0x43,0xa9,0x7a,0x7e,0x6a,
0x8e,0x50,0x4d,0x25,0x57,0xa1,0xbf,0x1b,0x15,0x05,0x43,0x7b,0x2c,0x05,0x8d,
0xbd,0x3d,0x03,0x8c,0x93,0x22,0x7d,0x63,0xea,0x0a,0x57,0x05,0x06,0x0a,0xdb,
0x61,0x98,0x65,0x2d,0x47,0x49,0xa8,0xe7,0xe6,0x56,0x75,0x5c,0xb8,0x64,0x08,
0x63,0xa9,0x30,0x40,0x66,0xb2,0xf9,0xb6,0xe3,0x34,0xe8,0x67,0x30,0xe1,0x43,
0x0b,0x87,0xff,0xc9,0xbe,0x72,0x10,0x5e,0x23,0xf0,0x9b,0xa7,0x48,0x65,0xbf,
0x09,0x88,0x7b,0xcd,0x72,0xbc,0x2e,0x79,0x9b,0x7b,0x02,0x03,0x01,0x00,0x01 };
static BYTE msPubKey2[] = {
0x30,0x82,0x01,0x0a,0x02,0x82,0x01,0x01,0x00,0xa9,0x02,0xbd,0xc1,0x70,0xe6,
0x3b,0xf2,0x4e,0x1b,0x28,0x9f,0x97,0x78,0x5e,0x30,0xea,0xa2,0xa9,0x8d,0x25,
0x5f,0xf8,0xfe,0x95,0x4c,0xa3,0xb7,0xfe,0x9d,0xa2,0x20,0x3e,0x7c,0x51,0xa2,
0x9b,0xa2,0x8f,0x60,0x32,0x6b,0xd1,0x42,0x64,0x79,0xee,0xac,0x76,0xc9,0x54,
0xda,0xf2,0xeb,0x9c,0x86,0x1c,0x8f,0x9f,0x84,0x66,0xb3,0xc5,0x6b,0x7a,0x62,
0x23,0xd6,0x1d,0x3c,0xde,0x0f,0x01,0x92,0xe8,0x96,0xc4,0xbf,0x2d,0x66,0x9a,
0x9a,0x68,0x26,0x99,0xd0,0x3a,0x2c,0xbf,0x0c,0xb5,0x58,0x26,0xc1,0x46,0xe7,
0x0a,0x3e,0x38,0x96,0x2c,0xa9,0x28,0x39,0xa8,0xec,0x49,0x83,0x42,0xe3,0x84,
0x0f,0xbb,0x9a,0x6c,0x55,0x61,0xac,0x82,0x7c,0xa1,0x60,0x2d,0x77,0x4c,0xe9,
0x99,0xb4,0x64,0x3b,0x9a,0x50,0x1c,0x31,0x08,0x24,0x14,0x9f,0xa9,0xe7,0x91,
0x2b,0x18,0xe6,0x3d,0x98,0x63,0x14,0x60,0x58,0x05,0x65,0x9f,0x1d,0x37,0x52,
0x87,0xf7,0xa7,0xef,0x94,0x02,0xc6,0x1b,0xd3,0xbf,0x55,0x45,0xb3,0x89,0x80,
0xbf,0x3a,0xec,0x54,0x94,0x4e,0xae,0xfd,0xa7,0x7a,0x6d,0x74,0x4e,0xaf,0x18,
0xcc,0x96,0x09,0x28,0x21,0x00,0x57,0x90,0x60,0x69,0x37,0xbb,0x4b,0x12,0x07,
0x3c,0x56,0xff,0x5b,0xfb,0xa4,0x66,0x0a,0x08,0xa6,0xd2,0x81,0x56,0x57,0xef,
0xb6,0x3b,0x5e,0x16,0x81,0x77,0x04,0xda,0xf6,0xbe,0xae,0x80,0x95,0xfe,0xb0,
0xcd,0x7f,0xd6,0xa7,0x1a,0x72,0x5c,0x3c,0xca,0xbc,0xf0,0x08,0xa3,0x22,0x30,
0xb3,0x06,0x85,0xc9,0xb3,0x20,0x77,0x13,0x85,0xdf,0x02,0x03,0x01,0x00,0x01 };
static BYTE msPubKey3[] = {
0x30,0x82,0x02,0x0a,0x02,0x82,0x02,0x01,0x00,0xf3,0x5d,0xfa,0x80,0x67,0xd4,
0x5a,0xa7,0xa9,0x0c,0x2c,0x90,0x20,0xd0,0x35,0x08,0x3c,0x75,0x84,0xcd,0xb7,
0x07,0x89,0x9c,0x89,0xda,0xde,0xce,0xc3,0x60,0xfa,0x91,0x68,0x5a,0x9e,0x94,
0x71,0x29,0x18,0x76,0x7c,0xc2,0xe0,0xc8,0x25,0x76,0x94,0x0e,0x58,0xfa,0x04,
0x34,0x36,0xe6,0xdf,0xaf,0xf7,0x80,0xba,0xe9,0x58,0x0b,0x2b,0x93,0xe5,0x9d,
0x05,0xe3,0x77,0x22,0x91,0xf7,0x34,0x64,0x3c,0x22,0x91,0x1d,0x5e,0xe1,0x09,
0x90,0xbc,0x14,0xfe,0xfc,0x75,0x58,0x19,0xe1,0x79,0xb7,0x07,0x92,0xa3,0xae,
0x88,0x59,0x08,0xd8,0x9f,0x07,0xca,0x03,0x58,0xfc,0x68,0x29,0x6d,0x32,0xd7,
0xd2,0xa8,0xcb,0x4b,0xfc,0xe1,0x0b,0x48,0x32,0x4f,0xe6,0xeb,0xb8,0xad,0x4f,
0xe4,0x5c,0x6f,0x13,0x94,0x99,0xdb,0x95,0xd5,0x75,0xdb,0xa8,0x1a,0xb7,0x94,
0x91,0xb4,0x77,0x5b,0xf5,0x48,0x0c,0x8f,0x6a,0x79,0x7d,0x14,0x70,0x04,0x7d,
0x6d,0xaf,0x90,0xf5,0xda,0x70,0xd8,0x47,0xb7,0xbf,0x9b,0x2f,0x6c,0xe7,0x05,
0xb7,0xe1,0x11,0x60,0xac,0x79,0x91,0x14,0x7c,0xc5,0xd6,0xa6,0xe4,0xe1,0x7e,
0xd5,0xc3,0x7e,0xe5,0x92,0xd2,0x3c,0x00,0xb5,0x36,0x82,0xde,0x79,0xe1,0x6d,
0xf3,0xb5,0x6e,0xf8,0x9f,0x33,0xc9,0xcb,0x52,0x7d,0x73,0x98,0x36,0xdb,0x8b,
0xa1,0x6b,0xa2,0x95,0x97,0x9b,0xa3,0xde,0xc2,0x4d,0x26,0xff,0x06,0x96,0x67,
0x25,0x06,0xc8,0xe7,0xac,0xe4,0xee,0x12,0x33,0x95,0x31,0x99,0xc8,0x35,0x08,
0x4e,0x34,0xca,0x79,0x53,0xd5,0xb5,0xbe,0x63,0x32,0x59,0x40,0x36,0xc0,0xa5,
0x4e,0x04,0x4d,0x3d,0xdb,0x5b,0x07,0x33,0xe4,0x58,0xbf,0xef,0x3f,0x53,0x64,
0xd8,0x42,0x59,0x35,0x57,0xfd,0x0f,0x45,0x7c,0x24,0x04,0x4d,0x9e,0xd6,0x38,
0x74,0x11,0x97,0x22,0x90,0xce,0x68,0x44,0x74,0x92,0x6f,0xd5,0x4b,0x6f,0xb0,
0x86,0xe3,0xc7,0x36,0x42,0xa0,0xd0,0xfc,0xc1,0xc0,0x5a,0xf9,0xa3,0x61,0xb9,
0x30,0x47,0x71,0x96,0x0a,0x16,0xb0,0x91,0xc0,0x42,0x95,0xef,0x10,0x7f,0x28,
0x6a,0xe3,0x2a,0x1f,0xb1,0xe4,0xcd,0x03,0x3f,0x77,0x71,0x04,0xc7,0x20,0xfc,
0x49,0x0f,0x1d,0x45,0x88,0xa4,0xd7,0xcb,0x7e,0x88,0xad,0x8e,0x2d,0xec,0x45,
0xdb,0xc4,0x51,0x04,0xc9,0x2a,0xfc,0xec,0x86,0x9e,0x9a,0x11,0x97,0x5b,0xde,
0xce,0x53,0x88,0xe6,0xe2,0xb7,0xfd,0xac,0x95,0xc2,0x28,0x40,0xdb,0xef,0x04,
0x90,0xdf,0x81,0x33,0x39,0xd9,0xb2,0x45,0xa5,0x23,0x87,0x06,0xa5,0x55,0x89,
0x31,0xbb,0x06,0x2d,0x60,0x0e,0x41,0x18,0x7d,0x1f,0x2e,0xb5,0x97,0xcb,0x11,
0xeb,0x15,0xd5,0x24,0xa5,0x94,0xef,0x15,0x14,0x89,0xfd,0x4b,0x73,0xfa,0x32,
0x5b,0xfc,0xd1,0x33,0x00,0xf9,0x59,0x62,0x70,0x07,0x32,0xea,0x2e,0xab,0x40,
0x2d,0x7b,0xca,0xdd,0x21,0x67,0x1b,0x30,0x99,0x8f,0x16,0xaa,0x23,0xa8,0x41,
0xd1,0xb0,0x6e,0x11,0x9b,0x36,0xc4,0xde,0x40,0x74,0x9c,0xe1,0x58,0x65,0xc1,
0x60,0x1e,0x7a,0x5b,0x38,0xc8,0x8f,0xbb,0x04,0x26,0x7c,0xd4,0x16,0x40,0xe5,
0xb6,0x6b,0x6c,0xaa,0x86,0xfd,0x00,0xbf,0xce,0xc1,0x35,0x02,0x03,0x01,0x00,
0x01 };

static BOOL WINAPI verify_ms_root_policy(LPCSTR szPolicyOID,
 PCCERT_CHAIN_CONTEXT pChainContext, PCERT_CHAIN_POLICY_PARA pPolicyPara,
 PCERT_CHAIN_POLICY_STATUS pPolicyStatus)
{
    BOOL ret = verify_base_policy(szPolicyOID, pChainContext, pPolicyPara,
     pPolicyStatus);

    if (ret && !pPolicyStatus->dwError)
    {
        CERT_PUBLIC_KEY_INFO msPubKey = { { 0 } };
        BOOL isMSRoot = FALSE;
        DWORD i;
        CRYPT_DATA_BLOB keyBlobs[] = {
         { sizeof(msPubKey1), msPubKey1 },
         { sizeof(msPubKey2), msPubKey2 },
         { sizeof(msPubKey3), msPubKey3 },
        };
        PCERT_SIMPLE_CHAIN rootChain =
         pChainContext->rgpChain[pChainContext->cChain -1 ];
        PCCERT_CONTEXT root =
         rootChain->rgpElement[rootChain->cElement - 1]->pCertContext;

        for (i = 0; !isMSRoot && i < sizeof(keyBlobs) / sizeof(keyBlobs[0]);
         i++)
        {
            msPubKey.PublicKey.cbData = keyBlobs[i].cbData;
            msPubKey.PublicKey.pbData = keyBlobs[i].pbData;
            if (CertComparePublicKeyInfo(
             X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
             &root->pCertInfo->SubjectPublicKeyInfo, &msPubKey))
                isMSRoot = TRUE;
        }
        if (isMSRoot)
            pPolicyStatus->lChainIndex = pPolicyStatus->lElementIndex = 0;
    }
    return ret;
}

typedef BOOL (WINAPI *CertVerifyCertificateChainPolicyFunc)(LPCSTR szPolicyOID,
 PCCERT_CHAIN_CONTEXT pChainContext, PCERT_CHAIN_POLICY_PARA pPolicyPara,
 PCERT_CHAIN_POLICY_STATUS pPolicyStatus);

BOOL WINAPI CertVerifyCertificateChainPolicy(LPCSTR szPolicyOID,
 PCCERT_CHAIN_CONTEXT pChainContext, PCERT_CHAIN_POLICY_PARA pPolicyPara,
 PCERT_CHAIN_POLICY_STATUS pPolicyStatus)
{
    static HCRYPTOIDFUNCSET set = NULL;
    BOOL ret = FALSE;
    CertVerifyCertificateChainPolicyFunc verifyPolicy = NULL;
    HCRYPTOIDFUNCADDR hFunc = NULL;

    TRACE("(%s, %p, %p, %p)\n", debugstr_a(szPolicyOID), pChainContext,
     pPolicyPara, pPolicyStatus);

    if (!HIWORD(szPolicyOID))
    {
        switch (LOWORD(szPolicyOID))
        {
        case LOWORD(CERT_CHAIN_POLICY_BASE):
            verifyPolicy = verify_base_policy;
            break;
        case LOWORD(CERT_CHAIN_POLICY_AUTHENTICODE):
            verifyPolicy = verify_authenticode_policy;
            break;
        case LOWORD(CERT_CHAIN_POLICY_SSL):
            verifyPolicy = verify_ssl_policy;
            break;
        case LOWORD(CERT_CHAIN_POLICY_BASIC_CONSTRAINTS):
            verifyPolicy = verify_basic_constraints_policy;
            break;
        case LOWORD(CERT_CHAIN_POLICY_MICROSOFT_ROOT):
            verifyPolicy = verify_ms_root_policy;
            break;
        default:
            FIXME("unimplemented for %d\n", LOWORD(szPolicyOID));
        }
    }
    if (!verifyPolicy)
    {
        if (!set)
            set = CryptInitOIDFunctionSet(
             CRYPT_OID_VERIFY_CERTIFICATE_CHAIN_POLICY_FUNC, 0);
        CryptGetOIDFunctionAddress(set, X509_ASN_ENCODING, szPolicyOID, 0,
         (void **)&verifyPolicy, &hFunc);
    }
    if (verifyPolicy)
        ret = verifyPolicy(szPolicyOID, pChainContext, pPolicyPara,
         pPolicyStatus);
    if (hFunc)
        CryptFreeOIDFunctionAddress(hFunc, 0);
    TRACE("returning %d (%08x)\n", ret, pPolicyStatus->dwError);
    return ret;
}
