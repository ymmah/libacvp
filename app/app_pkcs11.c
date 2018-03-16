/*****************************************************************************
* Copyright (c) 2016, Cisco Systems, Inc.
* All rights reserved.

* Redistribution and use in source and binary forms, with or without modification,
* are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
* USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/
/*
 * This module is not part of libacvp.  Rather, it's a simple app that
 * demonstrates how to use libacvp. Software that use libacvp
 * will need to implement a similar module.
 *
 * It will default to 127.0.0.1 port 443 if no arguments are given.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "acvp.h"
#ifdef USE_MURL
#include <murl/murl.h>
#else
#include <curl/curl.h>
#endif


#include "pkcs11_lcl.h"
#ifdef ENABLE_NSS_DRBG
#include "loader.h"
#else
#define FREEBLVector void
#endif

static void enable_aes(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list);
static void enable_tdes(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list);
static void enable_hash(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list);
static void enable_cmac(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list);
static void enable_hmac(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list);
static void enable_kdf(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list);
static void enable_dsa(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list);
static void enable_rsa(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list);
static void enable_drbg(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list);

static ACVP_RESULT pkcs11_aead_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT pkcs11_symetric_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT pkcs11_digest_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT pkcs11_keywrap_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT pkcs11_mac_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT pkcs11_rsa_handler(ACVP_TEST_CASE *test_case);
#ifdef IMPLEMENT
static ACVP_RESULT pkcs11_kdf_tls_handler(ACVP_TEST_CASE *test_case);
static ACVP_RESULT pkcs11_dsa_handler(ACVP_TEST_CASE *test_case);
#endif

static ACVP_RESULT app_drbg_handler(ACVP_TEST_CASE *test_case);

#define DEFAULT_SERVER "127.0.0.1"
#define DEFAULT_PORT 443
#define DEFAULT_CA_CHAIN "./certs/acvp-private-root-ca.crt.pem"
#define DEFAULT_CERT "./certs/sto-labsrv2-client-cert.pem"
#define DEFAULT_KEY "./certs/sto-labsrv2-client-key.pem"
#define DEFAULT_PKCS11_LIB "libsoftokn3.so"
char *server;
int port;
char *ca_chain_file;
char *cert_file; char *key_file; char *pkcs11_lib;
char *path_segment;
static int has_nss = 0;
CK_FUNCTION_LIST *pkcs11_function_list = NULL;
CK_SLOT_ID slot_id = -1;
char *slot_description = NULL;

static void
dump(FILE *file, unsigned char *c, int len) {
    int i;
    for (i=0; i < len ;i++) {
        fprintf(file, "%02x",c[i]);
    }
}

typedef enum {false=0, true=1 } bool;


#define CHECK_ENABLE_CAP_RV(rv, string) \
    if (rv != ACVP_SUCCESS) { \
        fprintf(stderr,"Failed to register " \
               #string" capability with libacvp (rv=%d)\n", rv); \
        unload_pkcs11(); \
        exit(1); \
    }

#define CHECK_ENABLE_CAP_RV2(rv, string, param) \
    if (rv != ACVP_SUCCESS) { \
        fprintf(stderr,"Failed to register " \
               #string" capability with libacvp (rv=%d)\n", param, rv); \
        unload_pkcs11(); \
        exit(1); \
    }

#define CHECK_CRYPTO_RV(rv, string) \
    if (rv != ACVP_SUCCESS) { \
        fprintf(stderr,"Failed "#string" (rv=%d)\n", rv); \
        return(rv); \
    }

#define CHECK_CRYPTO_RV_CLEANUP(rv, string) \
    if (rv != ACVP_SUCCESS) { \
        fprintf(stderr,"Failed "#string" (rv=%d)\n", rv); \
        goto cleanup; \
    }

/* sometimes we expect certain failures. If so drop out as normal, but don't
 * be noisy about it */
#define CHECK_CRYPTO_RV_EXPECTED_CLEANUP(rv, expected, string) \
    if (rv != ACVP_SUCCESS) { \
        if (rv != expected) fprintf(stderr,"Failed "#string" (rv=%d)\n", rv); \
        goto cleanup; \
    }

/*
 * Read the operational parameters from the various environment
 * variables.
 */
static void setup_session_parameters()
{
    char *tmp;

    server = getenv("ACV_SERVER");
    if (!server) server = DEFAULT_SERVER;

    tmp = getenv("ACV_PORT");
    if (tmp) port = atoi(tmp);
    if (!port) port = DEFAULT_PORT;

    path_segment = getenv("ACV_URI_PREFIX");
    if (!path_segment) path_segment = "";

    ca_chain_file = getenv("ACV_CA_FILE");
    if (!ca_chain_file) ca_chain_file = DEFAULT_CA_CHAIN;

    cert_file = getenv("ACV_CERT_FILE");
    if (!cert_file) cert_file = DEFAULT_CERT;

    key_file = getenv("ACV_KEY_FILE");
    if (!key_file) key_file = DEFAULT_KEY;

    pkcs11_lib = getenv("ACV_PKCS11_LIB");
    if (!pkcs11_lib) pkcs11_lib = DEFAULT_PKCS11_LIB;

    tmp = getenv("ACV_PKCS11_SLOT_ID");
    if (tmp) {
        slot_id = atoi(tmp);
    }

    slot_description = getenv("ACV_PKCS11_SLOT_DESCRIPTION");

    printf("Using the following parameters:\n\n");
    printf("    ACV_SERVER:     %s\n", server);
    printf("    ACV_PORT:       %d\n", port);
    printf("    ACV_URI_PREFIX: %s\n", path_segment);
    printf("    ACV_CA_FILE:    %s\n", ca_chain_file);
    printf("    ACV_CERT_FILE:  %s\n", cert_file);
    printf("    ACV_KEY_FILE:   %s\n", key_file);
    printf("    ACV_PKCS11_LIB: %s\n", pkcs11_lib);
    printf("    ACV_PKCS11_SLOT_ID: %ld\n", (long)slot_id);
    printf("    ACV_PKCS11_SLOT_DESCRIPTION: %s\n", slot_description);
}

/* For Windows, Use windows specific library loading and
 * symbol lookup here */
#include <dlfcn.h>
#include "pkcs11_server.h"
#include <sys/types.h>
#include <sys/wait.h>
static int pid = -1;
static int reply[2];
static int request[2];

/*
 * Load the PKCS #11 module
 */
ACVP_RESULT load_pkcs11(char *library_name)
{
    CK_C_GetFunctionList lc_GetFunctionList;
    void *library_handle = NULL;
    void *freebl_library_handle = NULL;
    const FREEBLVector *freebl_function_list = NULL;
    CK_RV crv;
    int rv = 0;
    ACVP_RESULT result;
#ifdef ENABLE_NSS_DRBG
    char *softoken;
#endif

    /* we fork in case we need to use the PKCS #11 module under test
     * to complete the curl function.
     */
    rv = pipe(request);
    if (rv < 0) {
        perror("pipe");
        fprintf(stderr,"Couldn't create a pipe\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    rv = pipe(reply);
    if (rv < 0) {
        perror("pipe");
        fprintf(stderr,"Couldn't create a pipe\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    pid = fork();
    if (pid < 0) {
        perror("fork");
        fprintf(stderr,"Couldn't fork\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    if (pid == 0) {
        /* close the parent's side of the pipe */
        close(request[1]);
        close(reply[0]);

        /* Child becomes the server, do all the PKCS #11 work */
            library_handle = dlopen(library_name,RTLD_LOCAL|RTLD_NOW);
            if (library_handle == NULL) {
            perror(library_name);
            fprintf(stderr,"%s: %s\n",library_name,dlerror());
            result = ACVP_INVALID_ARG;
            write(reply[1],&result, sizeof(result));
            exit(1);
        }
        lc_GetFunctionList=dlsym(library_handle,"C_GetFunctionList");
        if (lc_GetFunctionList == NULL) {
            perror(library_name);
            fprintf(stderr,"%s fetching C_GetFunctionList: %s\n",
                                library_name,dlerror());
            dlclose(library_handle);
            library_handle = NULL;
            result = ACVP_INVALID_ARG;
            write(reply[1],&result, sizeof(result));
            exit(1);
        }
        crv = (*lc_GetFunctionList)(&pkcs11_function_list);
        if (crv != CKR_OK) {
            fprintf(stderr, "C_GetFunctionList failed with ckrv=0x%lx\n",crv);
            dlclose(library_handle);
            library_handle = NULL;
            result = ACVP_INVALID_ARG;
            write(reply[1],&result, sizeof(result));
            exit(1);
        }


        /* handle the freebl case for NSS */
        has_nss = 0;
#ifdef ENABLE_NSS_DRBG
#define SOFTOKEN_NAME "softokn3"
#define FREEBL_NAME "freeblpriv3"
        if ((softoken = strstr(library_name,SOFTOKEN_NAME)) != NULL) {
            /* we're opening softoken, we also need to attach to freebl
             * to test the drbg */
            int softoken_name_start = softoken - library_name;
            int softoken_name_size = sizeof(SOFTOKEN_NAME)-1;
            int freebl_name_size = sizeof(FREEBL_NAME)-1;
            int tail_start = softoken_name_start + softoken_name_size;
            int library_name_size = strlen(library_name);
            int tail_size = library_name_size - tail_start;
            int freebl_size = library_name_size + freebl_name_size +
                                - softoken_name_size;
            char *freebl;
            FREEBLGetVectorFn *lfreebl_GetVector;


            freebl =  malloc(freebl_size+1);
            memcpy(freebl, library_name, softoken_name_start);
            memcpy(&freebl[softoken_name_start], FREEBL_NAME, freebl_name_size);
            memcpy(&freebl[softoken_name_start+freebl_name_size],
                                        &library_name[tail_start], tail_size);
            freebl[freebl_size] = 0;
                freebl_library_handle = dlopen(freebl,RTLD_LOCAL|RTLD_NOW);
            if (freebl_library_handle == NULL) {
                perror(library_name);
                fprintf(stderr,"%s: %s\n",freebl,dlerror());
                fprintf(stderr,"turning off DRBG tests \n");
                goto done;
            }
            lfreebl_GetVector=dlsym(freebl_library_handle,"FREEBL_GetVector");
            if (lc_GetFunctionList == NULL) {
                perror(freebl);
                fprintf(stderr,"%s fetching FREEBL_GetVector: %s\n",
                                        freebl,dlerror());
                fprintf(stderr,"turning off DRBG tests \n");
                dlclose(freebl_library_handle);
                freebl_library_handle = NULL;
                goto done;
            }
            freebl_function_list = (*lfreebl_GetVector)();
            if (freebl_function_list == NULL) {
                fprintf(stderr,"FREEBL_GetVector() failed.\n");
                fprintf(stderr,"turning off DRBG tests \n");
                dlclose(freebl_library_handle);
                freebl_library_handle = NULL;
                goto done;
            }
            has_nss = 1;
        }
done:
#endif
        result = ACVP_SUCCESS;
        write(reply[1],&result, sizeof(result));
        write(reply[1],&has_nss, sizeof(has_nss));
        pkcs11_server(request[0],reply[1], pkcs11_function_list,
                freebl_function_list, library_handle, freebl_library_handle);
        exit(0);
    }
    /* close the child's side of the pipe */
    close(reply[1]);
    close(request[0]);
    /* parent process becomes the client */
    read(reply[0],&result, sizeof(result));
    if (result != ACVP_SUCCESS) {
        int status;
        waitpid(pid, &status, 0);
        return result;
    }
    read(reply[0],&has_nss, sizeof(has_nss));
    pkcs11_client_set_fd(request[1],reply[0]);
    pkcs11_function_list = pkcs11_client_get_function_list();

    return ACVP_SUCCESS;
}

ACVP_RESULT unload_pkcs11()
{
    return pkcs11_client_close(pid);
}

/*
 * PKCS #11 helper function
 */
ACVP_RESULT pkcs11_init()
{
    CK_RV crv;
    CK_C_INITIALIZE_ARGS init_args;
    char *params = (char *)"configdir= flags=noModDB,noKeyDB,noCertDB,forceOpen";

    init_args.CreateMutex = NULL;
    init_args.DestroyMutex = NULL;
    init_args.LockMutex = NULL;
    init_args.UnlockMutex = NULL;
    init_args.flags = CKF_OS_LOCKING_OK; /* don't need any locking */
    init_args.LibraryParameters = (CK_CHAR_PTR *)params;
    init_args.pReserved = NULL;

    crv = (*pkcs11_function_list->C_Initialize)(&init_args);
    if (crv != CKR_OK) {
        /* this one is just informational */
        printf("NSS style init failed ckrv=0x%lx\nTrying traditional\n",crv);
        init_args.LibraryParameters = NULL;
        crv = (*pkcs11_function_list->C_Initialize)(&init_args);
        if (crv != CKR_OK) {
            fprintf(stderr,"C_Initialize failed ckrv=0x%lx\n",crv);
            return ACVP_CRYPTO_MODULE_FAIL;
        }
    }
    return ACVP_SUCCESS;
}

#ifdef SERVER_DRB_SUPPORT
bool has_nss_drbg() { return has_nss; }
#else
bool has_nss_drbg() { return 0; }
#endif

ACVP_RESULT pkcs11_get_info(CK_INFO *info)
{
    CK_RV crv;

    crv = (*pkcs11_function_list->C_GetInfo)(info);
    if (crv != CKR_OK) {
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}


/* compare a PKCS#11 UTF string with a NULL terminated string */
bool pkcs11_string_cmp(CK_UTF8CHAR *buf, int len, char *string)
{
   int slen = strlen(string)-1;
   int i;

   if (slen > len) {
        return false;
   }
   if (strncmp((char *)buf,string, slen) != 0) {
        return false;
   }
   for (i=slen; i < len; i++) {
        if ((buf[i] != 0) && (buf[i] != ' ')) {
            return false;
        }
    }
    return true;
}

/* format a PKCS#11 UTF string into a NULL terminated string */
const char *pkcs11_mk_string(CK_UTF8CHAR *buf, int len , char *space)
{
    char *last;
    memcpy(space,buf,len-1);
    space[len] = 0;
    for (last = &space[len-2];
                last != space && ((*last == 0) || (*last == ' '));
                last--)
                /* empty */ ;
    *(last+1)=0;
    return space;
}

/*
 * Try to find a slot id
 */
ACVP_RESULT pkcs11_get_slot()
{
    CK_RV crv;
    CK_ULONG count;
    CK_SLOT_ID static_list[10];
    CK_SLOT_ID *list;
    int i;

    list = &static_list[0];
    count = sizeof(static_list)/sizeof(static_list[0]);

    /* first get the list of slots. Only go for the slots that are present */
    crv = (*pkcs11_function_list->C_GetSlotList)(PR_TRUE,list,&count);
    if (crv == CKR_BUFFER_TOO_SMALL) {
        list = malloc(count*sizeof(CK_SLOT_ID));
        if (list == NULL) {
            return ACVP_MALLOC_FAIL;
        }
        crv = (*pkcs11_function_list->C_GetSlotList)(PR_TRUE,list,&count);
    }

    if (slot_id != -1) {
       for (i=0; i < count; i++) {
            if (slot_id == list[i]) {
                goto found;
            }
        }
        fprintf(stderr,"Slot ID read from the environment (%d) not found\n",
                                         (int)slot_id);
        slot_id = -1;
        return ACVP_INVALID_ARG;
     }

     if (slot_description) {
        /* search down the list of slots looking for the one with the given
         * description */
       for (i=0; i < count; i++) {
           CK_SLOT_INFO slot_info;
           CK_TOKEN_INFO token_info;
           crv = (*pkcs11_function_list->C_GetSlotInfo)(list[i],&slot_info);
           if (pkcs11_string_cmp(slot_info.slotDescription,
                        sizeof(slot_info.slotDescription),slot_description)) {
                slot_id = list[i];
                goto found;
           }
           /* do the same thing for token info */
           crv = (*pkcs11_function_list->C_GetTokenInfo)(list[i],&token_info);
           if (pkcs11_string_cmp(token_info.label, sizeof(token_info.label),
                                slot_description)) {
                slot_id = list[i];
                goto found;
           }
        }
        return ACVP_INVALID_ARG;
     }

     /* no slot specified in the environment, default to the first
      * one on the list */
     slot_id = list[0];
found:
     if (list != &static_list[0]) {
        free(list);
     }
     return ACVP_SUCCESS;
}


CK_MECHANISM_TYPE *pkcs11_get_mechanism_list()
{
    CK_MECHANISM_TYPE *list = NULL;
    CK_ULONG count = 0;
    CK_RV crv;

    crv = (*pkcs11_function_list->C_GetMechanismList)(slot_id, list, &count);
    list = malloc((count+1)*sizeof(CK_MECHANISM_TYPE));
    if (list == NULL) {
        return NULL;
    }
    crv = (*pkcs11_function_list->C_GetMechanismList)(slot_id, list, &count);
    if (crv != CKR_OK) {
        free(list);
        return NULL;
    }
    list[count] = CKM_INVALID_MECHANISM;
    return list;
}

ACVP_RESULT pkcs11_get_mechanism_info(CK_MECHANISM_TYPE mech,
                                      CK_MECHANISM_INFO *mech_info)
{
    CK_RV crv;
    crv = (*pkcs11_function_list->C_GetMechanismInfo)(slot_id, mech, mech_info);
    if (crv != CKR_OK) {
        fprintf(stderr,
                "C_GetMechanismInfo failed mech=%lx crv=%lx\n",mech, crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

bool pkcs11_has_mechanism(const CK_MECHANISM_TYPE *mech_list, CK_MECHANISM_TYPE mech)
{
   if (mech == CKM_INVALID_MECHANISM) {
        return false;
   }
   for (; *mech_list != CKM_INVALID_MECHANISM; mech_list++) {
        if (mech == *mech_list) {
            return true;
        }
    }
    return false;
}

CK_MECHANISM_TYPE pkcs11_get_mechanism(const CK_MECHANISM_TYPE *mech_list,
                                       const CK_MECHANISM_TYPE *test_list)
{
    for (; *test_list != CKM_INVALID_MECHANISM; test_list++) {
        if (pkcs11_has_mechanism(mech_list, *test_list)) {
            return *test_list;
        }
    }
    return CKM_INVALID_MECHANISM;
}


ACVP_SYM_CIPH_DIR pkcs11_get_direction(CK_ULONG flags)
{
    CK_ULONG tflags = 0;

    if ((flags & CKF_MESSAGE_ENCRYPT) || (flags & CKF_ENCRYPT)) {
        tflags |= CKF_ENCRYPT;
    }
    if ((flags & CKF_MESSAGE_DECRYPT) || (flags & CKF_DECRYPT)) {
        tflags |= CKF_DECRYPT;
    }
    switch (tflags) {
    case CKF_ENCRYPT:
        return ACVP_DIR_ENCRYPT;
    case CKF_DECRYPT:
        return ACVP_DIR_DECRYPT;
    case CKF_ENCRYPT|CKF_DECRYPT:
        return ACVP_DIR_BOTH;
    default:
        break;
    }
    fprintf(stderr,"Invalid encrypt/decrypt flags. Using just decrypt\n");
    return ACVP_DIR_DECRYPT;
}

#define BYTES 8
#define BITS 1
bool pkcs11_supports_key(CK_MECHANISM_INFO *mech_info, CK_ULONG key_len,
                        unsigned int scale)
{
     return (mech_info->ulMinKeySize*scale <= key_len) &&
                     (mech_info->ulMaxKeySize*scale >= key_len);
}

ACVP_RESULT pkcs11_open_session(CK_SLOT_ID slot_id, CK_SESSION_HANDLE *session)
{
    CK_RV crv;

    crv = (*pkcs11_function_list->C_OpenSession)(slot_id, 0, NULL, NULL,
                                 session);
    if (crv != CKR_OK) {
        fprintf(stderr,"C_OpenSession failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_close_session(CK_SESSION_HANDLE session)
{
    CK_RV crv;

    crv = (*pkcs11_function_list->C_CloseSession)(session);
    if (crv != CKR_OK) {
        fprintf(stderr,"C_CloseSession failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_get_attribute(CK_SESSION_HANDLE session,
     CK_OBJECT_HANDLE obj, CK_ATTRIBUTE_TYPE attr_type, unsigned char *data,
     unsigned int *length)
{
    CK_RV crv;
    CK_ATTRIBUTE attr = { attr_type, data, *length };

    crv = (*pkcs11_function_list->C_GetAttributeValue)(session, obj, &attr, 1);
    if (crv != CKR_OK) {
        fprintf(stderr,"C_GetAttributeValue failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    *length = attr.ulValueLen;
    return ACVP_SUCCESS;
}

static const CK_OBJECT_CLASS pkcs11_cko_key = CKO_SECRET_KEY;
static const CK_BBOOL pkcs11_ck_true = CK_TRUE;
static const CK_BBOOL pkcs11_ck_false = CK_FALSE;

CK_OBJECT_HANDLE pkcs11_import_sym_key(CK_SESSION_HANDLE session,
                                CK_KEY_TYPE key_type,
                                CK_ATTRIBUTE_TYPE operation,
                                unsigned char *key_data, int key_len)
{
    CK_OBJECT_HANDLE key = CK_INVALID_HANDLE;
    CK_RV crv;

    CK_ATTRIBUTE template[5];

    template[0].type = CKA_CLASS;
    template[0].pValue = (CK_OBJECT_CLASS *)&pkcs11_cko_key;
    template[0].ulValueLen = sizeof(pkcs11_cko_key);
    template[1].type = CKA_KEY_TYPE;
    template[1].pValue = &key_type;
    template[1].ulValueLen = sizeof(key_type);
    template[2].type = operation;
    template[2].pValue = (CK_BBOOL *)&pkcs11_ck_true;
    template[2].ulValueLen = sizeof(pkcs11_ck_true);
    template[3].type = CKA_TOKEN;
    template[3].pValue = (CK_BBOOL *)&pkcs11_ck_false;
    template[3].ulValueLen = sizeof(pkcs11_ck_false);
    template[4].type = CKA_VALUE;
    template[4].pValue = key_data;
    template[4].ulValueLen = key_len;

    crv = (*pkcs11_function_list->C_CreateObject)(session,
                                        &template[0], 5, &key);
    if (crv != CKR_OK) {
        fprintf(stderr,
        "Failed to create key:\n"
        "    class=0x%lx\n"
        "    type=0x%lx\n"
        "    operation=0x%lx\n"
        "    key_data=", pkcs11_cko_key, key_type, operation);
        dump(stderr,key_data,key_len);
        fprintf(stderr, "\n    key_len=%d\n",key_len);

        fprintf(stderr,"C_CreateObject failed ckrv=0x%lx\n",crv);
        return CK_INVALID_HANDLE;
    }
    return key;
}

ACVP_RESULT pkcs11_encrypt_init(CK_SESSION_HANDLE session,
                CK_MECHANISM *mech, CK_OBJECT_HANDLE key)
{
    CK_RV crv;

    crv = (*pkcs11_function_list->C_EncryptInit)(session, mech, key);
    if (crv != CKR_OK) {
        fprintf(stderr,"C_EncryptInit failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_encrypt_update(CK_SESSION_HANDLE session,
                unsigned char *pt, unsigned int pt_len,
                unsigned char *ct, unsigned int *ct_len)
{
    CK_RV crv;
    CK_ULONG ct_local = *ct_len;

    crv = (*pkcs11_function_list->C_EncryptUpdate)(session,
                pt, pt_len, ct, &ct_local);
    *ct_len = ct_local;
    if (crv != CKR_OK) {
        fprintf(stderr,"C_EncryptUpdate failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_encrypt(CK_SESSION_HANDLE session,
                unsigned char *pt, unsigned int pt_len,
                unsigned char *ct, unsigned int *ct_len)
{
    CK_RV crv;
    CK_ULONG ct_local = *ct_len;

    crv = (*pkcs11_function_list->C_Encrypt)(session,
                pt, pt_len, ct, &ct_local);
    *ct_len = ct_local;
    if (crv != CKR_OK) {
        fprintf(stderr,"C_Encrypt failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

typedef ACVP_RESULT (*pkcs11_final) (CK_SESSION_HANDLE session,
                unsigned char *, unsigned int *);

ACVP_RESULT pkcs11_encrypt_final(CK_SESSION_HANDLE session,
        unsigned char *buf, unsigned int *len_ptr)
{
    CK_RV crv;
    unsigned char dummy[128];
    CK_ULONG dummy_len = sizeof(dummy);

    if (len_ptr == NULL) {
        buf = dummy;
    } else {
        dummy_len = *len_ptr;
    }

    crv = (*pkcs11_function_list->C_EncryptFinal)(session,
                        &dummy[0], &dummy_len);
    if (len_ptr) *len_ptr = dummy_len;
    if (crv != CKR_OK) {
        fprintf(stderr,"C_EncryptFinal failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_decrypt_init(CK_SESSION_HANDLE session,
                CK_MECHANISM *mech, CK_OBJECT_HANDLE key)
{
    CK_RV crv;

    crv = (*pkcs11_function_list->C_DecryptInit)(session, mech, key);
    if (crv != CKR_OK) {
        fprintf(stderr,"C_DecryptInit failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_decrypt_update(CK_SESSION_HANDLE session,
                unsigned char *ct, unsigned int ct_len,
                unsigned char *pt, unsigned int *pt_len)
{
    CK_RV crv;
    CK_ULONG pt_local = *pt_len;

    crv = (*pkcs11_function_list->C_DecryptUpdate)(session,
                ct, ct_len, pt, &pt_local);
    *pt_len = pt_local;
    if (crv != CKR_OK) {
        fprintf(stderr,"C_DecryptUpdate failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_decrypt(CK_SESSION_HANDLE session,
                unsigned char *ct, unsigned int ct_len,
                unsigned char *pt, unsigned int *pt_len)
{
    CK_RV crv;
    CK_ULONG pt_local = *pt_len;

    crv = (*pkcs11_function_list->C_Decrypt)(session,
                ct, ct_len, pt, &pt_local);
    *pt_len = pt_local;
    if (crv == CKR_ENCRYPTED_DATA_INVALID) {
        return ACVP_CRYPTO_TAG_FAIL;
    }
    if (crv != CKR_OK) {
        fprintf(stderr,"C_Decrypt failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_decrypt_final(CK_SESSION_HANDLE session,
        unsigned char *buf, unsigned int *len_ptr)
{
    CK_RV crv;
    unsigned char dummy[128];
    CK_ULONG dummy_len = sizeof(dummy);

    if (len_ptr == NULL) {
        buf = dummy;
    } else {
        dummy_len = *len_ptr;
    }
    crv = (*pkcs11_function_list->C_DecryptFinal)(session,
                        &dummy[0], &dummy_len);
    if (len_ptr) *len_ptr = dummy_len;
    if (crv != CKR_OK) {
        fprintf(stderr,"C_DecryptFinal failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_digest_init(CK_SESSION_HANDLE session, CK_MECHANISM *mech)
{
    CK_RV crv;

    crv = (*pkcs11_function_list->C_DigestInit)(session, mech);
    if (crv != CKR_OK) {
        fprintf(stderr,"C_DigestInit failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_digest_update(CK_SESSION_HANDLE session,
                unsigned char *dt, unsigned int dt_len)
{
    CK_RV crv;

    crv = (*pkcs11_function_list->C_DigestUpdate)(session, dt, dt_len);
    if (crv != CKR_OK) {
        fprintf(stderr,"C_DigestUpdate failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_digest_final(CK_SESSION_HANDLE session,
                unsigned char *d, unsigned int *d_len )
{
    CK_RV crv;
    CK_ULONG d_local = *d_len;

    crv = (*pkcs11_function_list->C_DigestFinal)(session, d, &d_local);
    *d_len = d_local;
    if (crv != CKR_OK) {
        fprintf(stderr,"C_DigestFinal failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_sign_init(CK_SESSION_HANDLE session,
                CK_MECHANISM *mech, CK_OBJECT_HANDLE key)
{
    CK_RV crv;

    crv = (*pkcs11_function_list->C_SignInit)(session, mech, key);
    if (crv != CKR_OK) {
        fprintf(stderr,"C_SignInit failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_sign_update(CK_SESSION_HANDLE session,
                unsigned char *dt, unsigned int dt_len)
{
    CK_RV crv;

    crv = (*pkcs11_function_list->C_SignUpdate)(session, dt, dt_len);
    if (crv != CKR_OK) {
        fprintf(stderr,"C_SignUpdate failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

ACVP_RESULT pkcs11_sign_final(CK_SESSION_HANDLE session,
                unsigned char *d, unsigned int *d_len )
{
    CK_RV crv;
    CK_ULONG d_local = *d_len;

    crv = (*pkcs11_function_list->C_SignFinal)(session, d, &d_local);
    *d_len = d_local;
    if (crv != CKR_OK) {
        fprintf(stderr,"C_SignFinal failed ckrv=0x%lx\n",crv);
        return ACVP_CRYPTO_MODULE_FAIL;
    }
    return ACVP_SUCCESS;
}

/*
 * This is a minimal and rudimentary logging handler.
 * libacvp calls this function to for debugs, warnings,
 * and errors.
 */
ACVP_RESULT progress(char *msg)
{
    printf("ACVP Log: %s\n", msg);
    return ACVP_SUCCESS;
}

static void print_usage(char *prog)
{
    printf("\nInvalid usage...\n");
    printf("%s does not require any argument, however logging level can be\n",prog);
    printf("controlled using:\n");
    printf("      -none\n");
    printf("      -error(default)\n");
    printf("      -warn\n");
    printf("      -status\n");
    printf("      -info\n");
    printf("      -verbose\n");
    printf("\n");
    printf("In addition some options are passed to %s using\n",prog);
    printf("environment variables.  The following variables can be set:\n\n");
    printf("    ACV_SERVER (when not set, defaults to %s)\n", DEFAULT_SERVER);
    printf("    ACV_PORT (when not set, defaults to %d)\n", DEFAULT_PORT);
    printf("    ACV_URI_PREFIX (when not set, defaults to null)\n");
    printf("    ACV_CA_FILE (when not set, defaults to %s)\n", DEFAULT_CA_CHAIN);
    printf("    ACV_CERT_FILE (when not set, defaults to %s)\n", DEFAULT_CERT);
    printf("    ACV_KEY_FILE (when not set, defaults to %s)\n", DEFAULT_KEY);
    printf("    ACV_PKCS11_LIB (when not set, defaults to %s)\n", DEFAULT_PKCS11_LIB);
    printf("    ACV_PKCS11_SLOT_ID (when not set default AVC_PKCS11_SLOT_DESCRIPTION\n");
    printf("    ACV_PKCS11_SLOT_DESCRIPTION (when not set defaults to the first slot)\n\n");
    printf("The CA certificates, cert and key should be PEM encoded. There should be no\n");
    printf("password on the key file.\n");
}

int main(int argc, char **argv)
{
    ACVP_RESULT rv;
    ACVP_CTX *ctx;
    char library_version[10];
    CK_INFO info;
    char manufacturer[sizeof(info.manufacturerID)+1];
    char library_name[sizeof(info.libraryDescription)+1];
    CK_MECHANISM_TYPE *mech_list;
    ACVP_LOG_LVL level = ACVP_LOG_LVL_STATUS;
    char *prog = *argv;

    int aes = 1;
    int tdes = 1;
    int hash = 1;
    int cmac = 1;
    int hmac = 1;
    int kdf = 0;
    int dsa = 0;
    int rsa = 0;
    int drbg = 1;

    if (argc > 2) {
        print_usage(prog);
        return 1;
    }

    argv++;
    argc--;
    while (argc >= 1) {
        if (strcmp(*argv, "-info") == 0) {
            level = ACVP_LOG_LVL_INFO;
        }
        if (strcmp(*argv, "-status") == 0) {
            level = ACVP_LOG_LVL_STATUS;
        }
        if (strcmp(*argv, "-warn") == 0) {
            level = ACVP_LOG_LVL_WARN;
        }
        if (strcmp(*argv, "-error") == 0) {
            level = ACVP_LOG_LVL_ERR;
        }
        if (strcmp(*argv, "-none") == 0) {
            level = ACVP_LOG_LVL_NONE;
        }
        if (strcmp(*argv, "-verbose") == 0) {
            level = ACVP_LOG_LVL_VERBOSE;
        }
        if (strcmp(*argv, "-help") == 0) {
            print_usage(prog);
            return 1;
        }
    argv++;
    argc--;
    }

    setup_session_parameters();

    rv = load_pkcs11(pkcs11_lib);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to load library %s\n",pkcs11_lib);
        exit(1);
    }
    rv = pkcs11_init();
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to init library %s\n",pkcs11_lib);
        exit(1);
    }

    rv = pkcs11_get_info(&info);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to get module infor for token %s\n", pkcs11_lib);
        unload_pkcs11();
        exit(1);
    }

    /*
     * We begin the libacvp usage flow here.
     * First, we create a test session context.
     */
    rv = acvp_create_test_session(&ctx, &progress, level);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to create ACVP context\n");
        unload_pkcs11();
        exit(1);
    }

    /*
     * Next we specify the ACVP server address
     */
    rv = acvp_set_server(ctx, server, port);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to set server/port\n");
        unload_pkcs11();
        exit(1);
    }

    /*
     * Setup the vendor attributes
     */
    pkcs11_mk_string(info.manufacturerID, sizeof(manufacturer), manufacturer),
    rv = acvp_set_vendor_info(ctx, manufacturer,
        /* get these from the environment */
        "www.redhat.com", "Robert Relyea", "rrelyea@redhat.com");
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to set vendor info\n");
        unload_pkcs11();
        exit(1);
    }

    /*
     * Setup the crypto module attributes
     */
    snprintf(library_version, 10, "%d.%d",
                info.libraryVersion.major, info.libraryVersion.minor);
    pkcs11_mk_string(info.libraryDescription,
                         sizeof(library_name), library_name);
    rv = acvp_set_module_info(ctx, library_name, "software",
                                library_version, library_name);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to set module info\n");
        unload_pkcs11();
        exit(1);
    }

    /*
     * Set the path segment prefix if needed
     */
     if (strnlen(path_segment, 255) > 0) {
        rv = acvp_set_path_segment(ctx, path_segment);
        if (rv != ACVP_SUCCESS) {
            fprintf(stderr,"Failed to set URI prefix\n");
            unload_pkcs11();
            exit(1);
        }
     }

    /*
     * Next we provide the CA certs to be used by libacvp
     * to verify the ACVP TLS certificate.
     */
    rv = acvp_set_cacerts(ctx, ca_chain_file);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to set CA certs\n");
        unload_pkcs11();
        exit(1);
    }

    /*
     * Specify the certificate and private key the client should used
     * for TLS client auth.
     */
    rv = acvp_set_certkey(ctx, cert_file, key_file);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to set TLS cert/key\n");
        unload_pkcs11();
        exit(1);
    }


    rv = pkcs11_get_slot();
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr, "Couldn't find requested slot\n");
        unload_pkcs11();
        exit(1);
    }

    mech_list = pkcs11_get_mechanism_list();
    if (mech_list == NULL) {
        fprintf(stderr, "No mechanism found \n");
        unload_pkcs11();
        exit(1);
    }

    if (aes) {
        enable_aes(ctx, mech_list);
    }

    if (tdes) {
        enable_tdes(ctx, mech_list);
    }

    if (hash) {
        enable_hash(ctx, mech_list);
    }

    if (cmac) {
        enable_cmac(ctx, mech_list);
    }

    if (hmac) {
        enable_hmac(ctx, mech_list);
    }

    if (kdf) {
        enable_kdf(ctx, mech_list);
    }

    if (dsa) {
        enable_dsa(ctx, mech_list);
    }

    if (rsa) {
        enable_rsa(ctx, mech_list);
    }

    if (drbg) {
        enable_drbg(ctx, mech_list);
    }

    /*
     * Now that we have a test session, we register with
     * the server to advertise our capabilities and receive
     * the KAT vector sets the server demands that we process.
     */
    rv = acvp_register(ctx);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to register with ACVP server (rv=%d)\n", rv);
        unload_pkcs11();
        exit(1);
    }

    /*
     * Now we process the test cases given to us during
     * registration earlier.
     */
    rv = acvp_process_tests(ctx);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr,"Failed to process vectors (%d)\n", rv);
        unload_pkcs11();
        exit(1);
    }

    rv = acvp_check_test_results(ctx);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr, "Unable to retrieve test results (%d)\n", rv);
        unload_pkcs11();
        exit(1);
    }

    /*
     * Finally, we free the test session context and cleanup
     */
    rv = acvp_free_test_session(ctx);
    if (rv != ACVP_SUCCESS) {
        printf("Failed to free ACVP context\n");
        unload_pkcs11();
        exit(1);
    }
    unload_pkcs11();
    acvp_cleanup();

    return (0);
}

static void enable_aes (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list) {
   ACVP_RESULT rv;
    char value[] = "same";
   /*
     * We need to register all the crypto module capabilities that will be
     * validated.
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_GCM)) {
        ACVP_SYM_CIPH_IVGEN_SRC ivgen_src = ACVP_IVGEN_SRC_EXT;
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_GCM, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get AES_GCM Flags");

        /* PKCS #11 can't handle internal IV generation without using the AEAD
         * interface */
        if (mech_info.flags & (CKF_MESSAGE_ENCRYPT|CKF_MESSAGE_DECRYPT)) {
            ivgen_src = ACVP_IVGEN_SRC_INT;
        }

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_GCM,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ivgen_src, ACVP_IVGEN_MODE_822,
                &pkcs11_aead_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM");

#ifdef notdef
        rv = acvp_enable_prereq_cap(ctx, ACVP_AES_GCM, ACVP_PREREQ_AES, value);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM Prereq AES");
#endif

        /* only require DRBG if we can test the DRBG */
        if (has_nss_drbg()) {
            rv = acvp_enable_prereq_cap(ctx, ACVP_AES_GCM,
                                            ACVP_PREREQ_DRBG, value);
            CHECK_ENABLE_CAP_RV(rv, "AES GCM Prereq DRBG");
        }

        if (pkcs11_supports_key(&mech_info, 128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                 ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES GCM Keysize 128");
        }
        if (pkcs11_supports_key(&mech_info, 192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                 ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES GCM Keysize 192");
        }
        if (pkcs11_supports_key(&mech_info, 256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES GCM Keysize 256");
         }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_TAGLEN, 96);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM Tag Length 96");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_TAGLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM Tag Length 128");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_IVLEN, 96);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM IV Length 96");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_PTLEN, 0);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM Plain Text Length 0");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_PTLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM Plain Text Length 128");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_PTLEN, 136);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM Plain Text Length 136");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_AADLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM AAD Length 128");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_AADLEN, 136);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM AAD Length 136");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_GCM,
                ACVP_SYM_CIPH_AADLEN, 256);
        CHECK_ENABLE_CAP_RV(rv, "AES GCM AAD Length 256");
    }

    /*
     * Register AES CCM capabilities
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_CCM)) {
        ACVP_SYM_CIPH_IVGEN_SRC ivgen_src = ACVP_IVGEN_SRC_EXT;
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_CCM, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get AES_CCM Flags");

        /* PKCS #11 can't handle internal IV generation without using the AEAD
         * interface */
        if (mech_info.flags & (CKF_MESSAGE_ENCRYPT|CKF_MESSAGE_DECRYPT)) {
            ivgen_src = ACVP_IVGEN_SRC_INT;
        }
        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CCM,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ivgen_src, ACVP_IVGEN_MODE_822,
                &pkcs11_aead_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES CCM");
#ifdef notdef
        rv = acvp_enable_prereq_cap(ctx, ACVP_AES_CCM, ACVP_PREREQ_AES, value);
        CHECK_ENABLE_CAP_RV(rv, "AES CCM Prereq AES");
#endif
        if (pkcs11_supports_key(&mech_info, 128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM,
                                ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES CCM Keysize 128");
        }
        if (pkcs11_supports_key(&mech_info, 192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM,
                                ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES CCM Keysize 192");
        }
        if (pkcs11_supports_key(&mech_info, 256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM,
                                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES CCM Keysize 256");
        }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM,
                                ACVP_SYM_CIPH_TAGLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "AES CCM Tag Length 128");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM,
                                ACVP_SYM_CIPH_IVLEN, 56);
        CHECK_ENABLE_CAP_RV(rv, "AES CCM IV Length 56");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM,
                                ACVP_SYM_CIPH_PTLEN, 0);
        CHECK_ENABLE_CAP_RV(rv, "AES CCM Plain Text Length 0");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM,
                                ACVP_SYM_CIPH_PTLEN, 256);
        CHECK_ENABLE_CAP_RV(rv, "AES CCM Plain Text Length 256");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM,
                                ACVP_SYM_CIPH_AADLEN, 0);
        CHECK_ENABLE_CAP_RV(rv, "AES CCM AAD Length 0");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CCM,
                                ACVP_SYM_CIPH_AADLEN, 65536);
        CHECK_ENABLE_CAP_RV(rv, "AES CCM AAD Length 65536");
    }

    /*
     * Enable AES-CBC
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_CBC)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_CBC, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get AES_CBC Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CBC,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES CBC");

        if (pkcs11_supports_key(&mech_info, 128, BYTES)) {
             rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CBC,
                ACVP_SYM_CIPH_KEYLEN, 128);
             CHECK_ENABLE_CAP_RV(rv, "AES CBC Keysize 128");
        }
        if (pkcs11_supports_key(&mech_info, 192, BYTES)) {
             rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CBC,
                ACVP_SYM_CIPH_KEYLEN, 192);
             CHECK_ENABLE_CAP_RV(rv, "AES CBC Keysize 192");
        }
        if (pkcs11_supports_key(&mech_info, 256, BYTES)) {
             rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CBC,
                ACVP_SYM_CIPH_KEYLEN, 256);
             CHECK_ENABLE_CAP_RV(rv, "AES CBC Keysize 256");
        }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CBC,
                ACVP_SYM_CIPH_PTLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "AES CBC Plain Text Length 128");
    }

    /*
     * Enable AES-ECB
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_ECB)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_ECB, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get AES_ECB Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_ECB,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES ECB");
        if (pkcs11_supports_key(&mech_info, 128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_ECB,
                ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES ECB Keysize 128");
         }
        if (pkcs11_supports_key(&mech_info, 192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_ECB,
                ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES ECB Keysize 192");
         }
        if (pkcs11_supports_key(&mech_info,256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_ECB,
                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES ECB Keysize 256");
         }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_ECB,
                ACVP_SYM_CIPH_PTLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "AES ECB Plain Text Length 128");
    }

    /*
     * Enable AES-CFB8
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_CFB8)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_CFB8, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get AES_CFB8 Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CFB8,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES CFB8");
        if (pkcs11_supports_key(&mech_info, 128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB8,
                ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES CFB8 Keysize 128");
         }
        if (pkcs11_supports_key(&mech_info, 192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB8,
                ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES CFB8 Keysize 192");
         }
        if (pkcs11_supports_key(&mech_info,256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB8,
                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES CFB8 Keysize 256");
         }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB8,
                ACVP_SYM_CIPH_PTLEN, 256);
        CHECK_ENABLE_CAP_RV(rv, "AES CFB8 Plain Text Length 256");
    }

    /*
     * Enable AES-CFB1
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_CFB1)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_CFB1, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get AES_CFB1 Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CFB1,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES CFB1");
        if (pkcs11_supports_key(&mech_info, 128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB1,
                ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES CFB1 Keysize 128");
         }
        if (pkcs11_supports_key(&mech_info, 192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB1,
                ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES CFB1 Keysize 192");
         }
        if (pkcs11_supports_key(&mech_info,256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB1,
                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES CFB1 Keysize 256");
         }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB1,
                ACVP_SYM_CIPH_PTLEN, 1536);
        CHECK_ENABLE_CAP_RV(rv, "AES CFB1 Plain Text Length 1536");
    }

    /*
     * Enable AES-CFB128
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_CFB128)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_CFB128, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get AES_CFB128 Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CFB128,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES CFB128");
        if (pkcs11_supports_key(&mech_info, 128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB128,
                ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES CFB128 Keysize 128");
         }
        if (pkcs11_supports_key(&mech_info, 192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB128,
                ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES CFB128 Keysize 192");
         }
        if (pkcs11_supports_key(&mech_info,256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB128,
                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES CFB128 Keysize 256");
         }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CFB128,
                ACVP_SYM_CIPH_PTLEN, 1536);
        CHECK_ENABLE_CAP_RV(rv, "AES CFB128 Plain Text Length 1536");
    }

    /*
     * Enable AES-OFB
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_OFB)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_OFB, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get AES_OFB Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_OFB,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES OFB");
        if (pkcs11_supports_key(&mech_info, 128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_OFB,
                ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES OFB Keysize 128");
         }
        if (pkcs11_supports_key(&mech_info, 192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_OFB,
                ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES OFB Keysize 192");
         }
        if (pkcs11_supports_key(&mech_info,256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_OFB,
                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES OFB Keysize 256");
         }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_OFB,
                ACVP_SYM_CIPH_PTLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "AES OFB Plain Text Length 128");
    }

#ifdef ACVP_V05
    /*
     * Enable AES-CTR
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_CTR)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_CTR, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get AES_CTR Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_CTR,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES CTR");
        if (pkcs11_supports_key(&mech_info, 128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CTR,
                ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES CTR Keysize 128");
         }
        if (pkcs11_supports_key(&mech_info, 192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CTR,
                ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES CTR Keysize 192");
         }
        if (pkcs11_supports_key(&mech_info, 256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CTR,
                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES CTR Keysize 256");
         }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_CTR,
                ACVP_SYM_CIPH_PTLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "AES CTR Plain Text Length 128");
    }
#endif

    /*
     * Enable AES keywrap for various key sizes and PT lengths
     * Note: this is with padding disabled, minimum PT length is 128 bits
     * and must be a multiple of 64 bits.
     */
    if (pkcs11_has_mechanism(mech_list, CKM_AES_KEY_WRAP)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_KEY_WRAP, &mech_info);
        CHECK_ENABLE_CAP_RV(rv, "Get AES_KEY_WRAP Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_KW,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_keywrap_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP");
        rv = acvp_enable_sym_cipher_cap_value(ctx, ACVP_AES_KW,
                ACVP_SYM_CIPH_KW_MODE, ACVP_SYM_KW_CIPHER);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP Mode=cipher");

        if (pkcs11_supports_key(&mech_info,128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW,
                ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP Keysize 128");
         }
        if (pkcs11_supports_key(&mech_info,192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW,
                ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP Keysize 192");
         }
        if (pkcs11_supports_key(&mech_info,256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW,
                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP Keysize 256");
         }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW,
                ACVP_SYM_CIPH_PTLEN, 512);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP Plain Text Length 512");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW,
                ACVP_SYM_CIPH_PTLEN, 192);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP Plain Text Length 192");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KW,
                ACVP_SYM_CIPH_PTLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP Plain Text Length 128");
    }

    if (pkcs11_has_mechanism(mech_list, CKM_AES_KEY_WRAP_PAD)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_AES_KEY_WRAP_PAD, &mech_info);
        CHECK_ENABLE_CAP_RV(rv, "Get AES_KEY_WRAP_PAD Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_AES_KWP,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_NA, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_keywrap_handler);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP P");
        rv = acvp_enable_sym_cipher_cap_value(ctx, ACVP_AES_KWP,
                ACVP_SYM_CIPH_KW_MODE, ACVP_SYM_KW_CIPHER);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP P Mode=cipher");

        if (pkcs11_supports_key(&mech_info,128, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KWP,
                ACVP_SYM_CIPH_KEYLEN, 128);
            CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP P Keysize 128");
         }
        if (pkcs11_supports_key(&mech_info,192, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KWP,
                ACVP_SYM_CIPH_KEYLEN, 192);
            CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP P Keysize 192");
         }
        if (pkcs11_supports_key(&mech_info,256, BYTES)) {
            rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KWP,
                ACVP_SYM_CIPH_KEYLEN, 256);
            CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP P Keysize 256");
         }
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KWP,
                ACVP_SYM_CIPH_PTLEN, 8);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP P Plain Text Length 8");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KWP,
                ACVP_SYM_CIPH_PTLEN, 32);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP P Plain Text Length 32");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KWP,
                ACVP_SYM_CIPH_PTLEN, 96);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP P Plain Text Length 96");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_AES_KWP,
                ACVP_SYM_CIPH_PTLEN, 808);
        CHECK_ENABLE_CAP_RV(rv, "AES KEY WRAP P Plain Text Length 808");
    }
}


static void enable_tdes (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list) {
   ACVP_RESULT rv;
    /*
     * Enable 3DES-ECB
     */
    if (pkcs11_has_mechanism(mech_list, CKM_DES3_ECB)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_DES3_ECB, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get TDES_ECB Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_ECB,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "TDES ECB");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_ECB,
                ACVP_SYM_CIPH_KEYLEN, 192);
        CHECK_ENABLE_CAP_RV(rv, "TDES ECB Keysize 192");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_ECB, ACVP_SYM_CIPH_PTLEN, 16*8*4);
        CHECK_ENABLE_CAP_RV(rv,"TDES ECB Plain Text 16*8*4");
    }

    /*
     * Enable 3DES-CBC
     */
    if (pkcs11_has_mechanism(mech_list, CKM_DES3_CBC)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_DES3_CBC, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get TDES_CBC Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_CBC,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv,"TDES CBC");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC,
                ACVP_SYM_CIPH_KEYLEN, 192);
        CHECK_ENABLE_CAP_RV(rv,"TDES CBC Keysize 192");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC,
                ACVP_SYM_CIPH_IVLEN, 192/3);
        CHECK_ENABLE_CAP_RV(rv,"TDES CBC IV Length 192/3");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC,
                ACVP_SYM_CIPH_PTLEN, 64);
        CHECK_ENABLE_CAP_RV(rv,"TDES CBC Plain Text Length 64");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC,
                ACVP_SYM_CIPH_PTLEN, 64*2);
        CHECK_ENABLE_CAP_RV(rv,"TDES CBC Plain Text Length 64*2");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC,
                ACVP_SYM_CIPH_PTLEN, 64*3);
        CHECK_ENABLE_CAP_RV(rv,"TDES CBC Plain Text Length 64*3");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CBC,
                ACVP_SYM_CIPH_PTLEN, 64*12);
        CHECK_ENABLE_CAP_RV(rv,"TDES CBC Plain Text Length 64*12");
    }

    /*
     * Enable 3DES-OFB
     */
    if (pkcs11_has_mechanism(mech_list, CKM_DES3_OFB8)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_DES3_OFB8, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get TDES_OFB Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_OFB,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "TDES OFB");
        rv = acvp_enable_sym_cipher_cap_parm(ctx,
                ACVP_TDES_OFB, ACVP_SYM_CIPH_KEYLEN, 192);
        CHECK_ENABLE_CAP_RV(rv, "TDES OFB Keysize 192");
        rv = acvp_enable_sym_cipher_cap_parm(ctx,
                ACVP_TDES_OFB, ACVP_SYM_CIPH_IVLEN, 192/3);
        CHECK_ENABLE_CAP_RV(rv, "TDES OFB IV Length 192/3");
        rv = acvp_enable_sym_cipher_cap_parm(ctx,
                ACVP_TDES_OFB, ACVP_SYM_CIPH_PTLEN, 64);
        CHECK_ENABLE_CAP_RV(rv, "TDES OFB Plain Text Length 64");
    }

    /*
     * Enable 3DES-CFB64
     */
    if (pkcs11_has_mechanism(mech_list, CKM_DES3_CFB64)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_DES3_CFB64, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get TDES_CFB64 Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_CFB64,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 64");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB64,
                ACVP_SYM_CIPH_KEYLEN, 192);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 64 Keysize 192");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB64,
                ACVP_SYM_CIPH_IVLEN, 192/3);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 64 IV Length 192/3");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB64,
                ACVP_SYM_CIPH_PTLEN, 64 * 5);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 64 Plain Text Length 64*5");
    }

    /*
     * Enable 3DES-CFB8
     */
    if (pkcs11_has_mechanism(mech_list, CKM_DES3_CFB8)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_DES3_CFB8, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get TDES_CFB8 Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_CFB8,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 8");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB8,
                ACVP_SYM_CIPH_KEYLEN, 192);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 8 Keysize 192");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB8,
                ACVP_SYM_CIPH_IVLEN, 192/3);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 8 IV Length 192/3");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB8,
                ACVP_SYM_CIPH_PTLEN, 64);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 8 Plain Text Length 64");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB8,
                ACVP_SYM_CIPH_PTLEN, 64 * 4);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 8 Plain Text Length 64*4");
    }

    /*
     * Enable 3DES-CFB1
     */
    if (pkcs11_has_mechanism(mech_list, CKM_DES3_CFB1)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_DES3_CFB1, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get TDES_CFB1 Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_CFB1,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 1");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB1,
                ACVP_SYM_CIPH_KEYLEN, 192);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 1 Keysize 192");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB1,
                ACVP_SYM_CIPH_IVLEN, 192/3);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 1 IV Length 192/3");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CFB1,
                ACVP_SYM_CIPH_PTLEN, 64);
        CHECK_ENABLE_CAP_RV(rv, "TDES CFB 1 Plain Text Length 64");
    }

    /*
     * Enable 3DES-CTR
     */
    if (pkcs11_has_mechanism(mech_list, CKM_DES3_CTR)) {
        CK_MECHANISM_INFO mech_info;

        rv = pkcs11_get_mechanism_info(CKM_DES3_CTR, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get TDES_CTR Flags");

        rv = acvp_enable_sym_cipher_cap(ctx, ACVP_TDES_CTR,
                pkcs11_get_direction(mech_info.flags),
                ACVP_KO_THREE, ACVP_IVGEN_SRC_NA, ACVP_IVGEN_MODE_NA,
                &pkcs11_symetric_handler);
        CHECK_ENABLE_CAP_RV(rv, "TDES CTR");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CTR,
                ACVP_SYM_CIPH_KEYLEN, 192);
        CHECK_ENABLE_CAP_RV(rv, "TDES CTR Keysize 192");
        rv = acvp_enable_sym_cipher_cap_parm(ctx, ACVP_TDES_CTR,
                ACVP_SYM_CIPH_PTLEN, 64);
        CHECK_ENABLE_CAP_RV(rv, "TDES CTR Plain Text Length 64");
    }
}

static void enable_hash (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list) {
    ACVP_RESULT rv;

    /*
     * Enable SHA-1 and SHA-2
     */
    if (pkcs11_has_mechanism(mech_list, CKM_SHA_1)) {
        rv = acvp_enable_hash_cap(ctx, ACVP_SHA1, &pkcs11_digest_handler);
        CHECK_ENABLE_CAP_RV(rv, "SHA 1");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA1, ACVP_HASH_IN_BIT, 0);
        CHECK_ENABLE_CAP_RV(rv, "SHA 1 Hash In Bit 0");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA1, ACVP_HASH_IN_EMPTY, 1);
        CHECK_ENABLE_CAP_RV(rv, "SHA 1 Hash In Empty 1");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA224)) {
        rv = acvp_enable_hash_cap(ctx, ACVP_SHA224, &pkcs11_digest_handler);
        CHECK_ENABLE_CAP_RV(rv, "SHA 224");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA224, ACVP_HASH_IN_BIT, 0);
        CHECK_ENABLE_CAP_RV(rv, "SHA 224 Hash In Bit 0");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA224, ACVP_HASH_IN_EMPTY, 1);
        CHECK_ENABLE_CAP_RV(rv, "SHA 224 Hash In Empty 1");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA256)) {
        rv = acvp_enable_hash_cap(ctx, ACVP_SHA256, &pkcs11_digest_handler);
        CHECK_ENABLE_CAP_RV(rv, "SHA 256");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA256, ACVP_HASH_IN_BIT, 0);
        CHECK_ENABLE_CAP_RV(rv, "SHA 256 Hash In Bit 0");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA256, ACVP_HASH_IN_EMPTY, 1);
        CHECK_ENABLE_CAP_RV(rv, "SHA 256 Hash In Empty 1");
    }

    if (pkcs11_has_mechanism(mech_list, CKM_SHA384)) {
        rv = acvp_enable_hash_cap(ctx, ACVP_SHA384, &pkcs11_digest_handler);
        CHECK_ENABLE_CAP_RV(rv, "SHA 384");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA384, ACVP_HASH_IN_BIT, 0);
        CHECK_ENABLE_CAP_RV(rv, "SHA 384 Hash In Bit 0");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA384, ACVP_HASH_IN_EMPTY, 1);
        CHECK_ENABLE_CAP_RV(rv, "SHA 384 Hash In Empty 1");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA512)) {
        rv = acvp_enable_hash_cap(ctx, ACVP_SHA512, &pkcs11_digest_handler);
        CHECK_ENABLE_CAP_RV(rv, "SHA 512");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA512, ACVP_HASH_IN_BIT, 0);
        CHECK_ENABLE_CAP_RV(rv, "SHA 512 Hash In Bit 0");
        rv = acvp_enable_hash_cap_parm(ctx, ACVP_SHA512, ACVP_HASH_IN_EMPTY, 1);
        CHECK_ENABLE_CAP_RV(rv, "SHA 512 Hash In Empty 1");
    }
}

static void enable_hmac (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list) {
    ACVP_RESULT rv;
    CK_MECHANISM_INFO mech_info;
    char value[] = "same";

    /*
     * Enable SHA-1 and SHA-2
     */
    if (pkcs11_has_mechanism(mech_list, CKM_SHA_1_HMAC)) {
        rv = pkcs11_get_mechanism_info(CKM_SHA_1_HMAC, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get HMAC_SHA_1");
        rv = acvp_enable_hash_cap(ctx, ACVP_HMAC_SHA1, &pkcs11_mac_handler);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA 1");
/* should use mech_info here, but softoken reports 1-128
 * but doesn't enforce it*/
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1,
                ACVP_HMAC_KEYLEN_MIN, 32 * 8);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA 1 Min Keysize");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1,
                ACVP_HMAC_KEYLEN_MAX, 56 * 8);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA 1 Max Keysize");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA1,
                                       ACVP_HMAC_MACLEN, 160);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA 1 Mac Len 160");
        rv = acvp_enable_prereq_cap(ctx, ACVP_HMAC_SHA1,
                                         ACVP_PREREQ_SHA, value);
        CHECK_ENABLE_CAP_RV(rv, "MAC SHA 1 Prereq SHA");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA224_HMAC)) {
        rv = pkcs11_get_mechanism_info(CKM_SHA224_HMAC, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get HMAC_SHA_224");
        rv = acvp_enable_hash_cap(ctx, ACVP_HMAC_SHA2_224, &pkcs11_mac_handler);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA 224");
/* should use mech_info here, but softoken reports 1-128
 * but doesn't enforce it*/
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224,
                ACVP_HMAC_KEYLEN_MIN, 32 * 8);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA224 Min Keysize");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224,
                ACVP_HMAC_KEYLEN_MAX, 56 * 8);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA224 Max Keysize");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_224,
                                       ACVP_HMAC_MACLEN, 224);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA224 Mac Len 224");
        rv = acvp_enable_prereq_cap(ctx, ACVP_HMAC_SHA2_224,
                                         ACVP_PREREQ_SHA, value);
        CHECK_ENABLE_CAP_RV(rv, "MAC SHA224 Prereq SHA");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA256_HMAC)) {
        rv = pkcs11_get_mechanism_info(CKM_SHA256_HMAC, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get HMAC_SHA_256");
        rv = acvp_enable_hash_cap(ctx, ACVP_HMAC_SHA2_256, &pkcs11_mac_handler);
        CHECK_ENABLE_CAP_RV(rv, "SHA 256");
/* should use mech_info here, but softoken reports 1-128
 * but doesn't enforce it*/
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256,
                ACVP_HMAC_KEYLEN_MIN, 32 * 8);
        CHECK_ENABLE_CAP_RV(rv, "SHA256 Min Keysize");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256,
                ACVP_HMAC_KEYLEN_MAX, 56 * 8);
        CHECK_ENABLE_CAP_RV(rv, "SHA256 Max Keysize");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_256,
                                       ACVP_HMAC_MACLEN, 256);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA256 Mac Len 256");
        rv = acvp_enable_prereq_cap(ctx, ACVP_HMAC_SHA2_256,
                                         ACVP_PREREQ_SHA, value);
        CHECK_ENABLE_CAP_RV(rv, "MAC SHA256 Prereq SHA");
    }

    if (pkcs11_has_mechanism(mech_list, CKM_SHA384_HMAC)) {
        rv = pkcs11_get_mechanism_info(CKM_SHA384_HMAC, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get HMAC_SHA_384");
        rv = acvp_enable_hash_cap(ctx, ACVP_HMAC_SHA2_384, &pkcs11_mac_handler);
        CHECK_ENABLE_CAP_RV(rv, "SHA 384");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384,
                                       ACVP_HMAC_MACLEN, 384);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA384 Mac Len 384");
/* should use mech_info here, but softoken reports 1-128
 * but doesn't enforce it*/
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384,
                ACVP_HMAC_KEYLEN_MIN, 32 * 8);
        CHECK_ENABLE_CAP_RV(rv, "SHA384 Min Keysize");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_384,
                ACVP_HMAC_KEYLEN_MAX, 56 * 8);
        CHECK_ENABLE_CAP_RV(rv, "SHA384 Max Keysize");
        rv = acvp_enable_prereq_cap(ctx, ACVP_HMAC_SHA2_384,
                                         ACVP_PREREQ_SHA, value);
        CHECK_ENABLE_CAP_RV(rv, "MAC SHA384 Prereq SHA");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA512_HMAC)) {
        rv = pkcs11_get_mechanism_info(CKM_SHA512_HMAC, &mech_info);
        CHECK_ENABLE_CAP_RV(rv,"Get HMAC_SHA_512");
        rv = acvp_enable_hash_cap(ctx, ACVP_HMAC_SHA2_512, &pkcs11_mac_handler);
        CHECK_ENABLE_CAP_RV(rv, "SHA 512");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512,
                                       ACVP_HMAC_MACLEN, 512);
        CHECK_ENABLE_CAP_RV(rv, "HMAC SHA512 Mac Len 512");
/* should use mech_info here, but softoken reports 1-128
 * but doesn't enforce it*/
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512,
                ACVP_HMAC_KEYLEN_MIN, 32 * 8);
        CHECK_ENABLE_CAP_RV(rv, "SHA512 Min Keysize");
        rv = acvp_enable_hmac_cap_parm(ctx, ACVP_HMAC_SHA2_512,
                ACVP_HMAC_KEYLEN_MAX, 56 * 8);
        CHECK_ENABLE_CAP_RV(rv, "SHA512 Max Keysize");
        rv = acvp_enable_prereq_cap(ctx, ACVP_HMAC_SHA2_512,
                                         ACVP_PREREQ_SHA, value);
        CHECK_ENABLE_CAP_RV(rv, "MAC SHA512 Prereq SHA");
    }
}

static void enable_cmac (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list) {
    ACVP_RESULT rv;
    char value[] = "same";
#ifdef notdef
    if (pkcs11_has_mechanism(mech_list, CKM_AES_CMAC)) {
        rv = acvp_enable_cmac_cap(ctx, ACVP_CMAC_AES, &pkcs11_mac_handler);
        CHECK_ENABLE_CAP_RV(rv, "CMAC AES");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES,
                                       ACVP_CMAC_BLK_DIVISIBLE_1, 1024);
        CHECK_ENABLE_CAP_RV(rv, "CMAC AES Block Divisible 1024");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES,
                                       ACVP_CMAC_BLK_NOT_DIVISIBLE_1, 2048);
        CHECK_ENABLE_CAP_RV(rv, "CMAC AES Block Not Divisible 2048");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES,
                                       ACVP_CMAC_MACLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "CMAC AES MAC Len 128");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES,
                                       ACVP_CMAC_KEYLEN, 128);
        CHECK_ENABLE_CAP_RV(rv, "CMAC AES Keysize 128");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES,
                                       ACVP_CMAC_DIRECTION_GEN, 1);
        CHECK_ENABLE_CAP_RV(rv, "CMAC AES Direction Gen 1");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_AES,
                                       ACVP_CMAC_DIRECTION_VER, 1);
        CHECK_ENABLE_CAP_RV(rv, "CMAC AES Direction Ver 1");
#ifdef notdef
        rv = acvp_enable_prereq_cap(ctx, ACVP_CMAC_AES, ACVP_PREREQ_AES, value);
        CHECK_ENABLE_CAP_RV(rv, "CMAC AES Prereq AES");
#endif
    }
#endif
    if (pkcs11_has_mechanism(mech_list, CKM_DES3_CMAC)) {
        rv = acvp_enable_cmac_cap(ctx, ACVP_CMAC_TDES, &pkcs11_mac_handler);
        CHECK_ENABLE_CAP_RV(rv, "CMAC TDES");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_TDES,
                                       ACVP_CMAC_BLK_DIVISIBLE_1, 1024);
        CHECK_ENABLE_CAP_RV(rv, "CMAC TDES Block Divisible 1024");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_TDES,
                                       ACVP_CMAC_BLK_NOT_DIVISIBLE_1, 2048);
        CHECK_ENABLE_CAP_RV(rv, "CMAC TDES Block Not Divisible 2048");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_TDES,
                                       ACVP_CMAC_MACLEN, 64);
        CHECK_ENABLE_CAP_RV(rv, "CMAC TDES MAC Len 128");
        /*rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_TDES,
                                       ACVP_CMAC_KEYING_OPTION, 1);
        CHECK_ENABLE_CAP_RV(rv, "CMAC TDES Keying Option 1"); */
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_TDES,
                                       ACVP_CMAC_DIRECTION_GEN, 1);
        CHECK_ENABLE_CAP_RV(rv, "CMAC TDES Direction Gen 1");
        rv = acvp_enable_cmac_cap_parm(ctx, ACVP_CMAC_TDES,
                                       ACVP_CMAC_DIRECTION_VER, 0);
        CHECK_ENABLE_CAP_RV(rv, "CMAC TDES Direction Ver 1");
        rv = acvp_enable_prereq_cap(ctx, ACVP_CMAC_TDES,
                                       ACVP_PREREQ_TDES, value);
        CHECK_ENABLE_CAP_RV(rv, "CMAC TDES Prereq TDES");
    }
}

#ifdef notdef
ACVP_KDF135_TLS_CAP_PARAM pkcs11_get_tls_hash(CK_MECHANISM_TYPE *mech_list) {
    ACVP_KDF135_TLS_CAP_PARAM int hashPrf = 0;
    if (pkcs11_has_mechanism(mech_list, CKM_SHA256)) {
        hashPrf |= ACVP_KDF135_TLS_CAP_SHA256;
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA384)) {
        hashPrf |= ACVP_KDF135_TLS_CAP_SHA384;
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA512)) {
        hashPrf |= ACVP_KDF135_TLS_CAP_SHA512;
    }
    return hashPrf;
}
#endif

static void enable_kdf (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list) {
#ifdef nodef
    ACVP_RESULT rv;
    ACVP_KDF135_TLS_CAP_PARAM int hashPrf = pkcs11_get_tls_hash(mech_list);
    int hasTLS10 = 0;
    int hasTLS12 = 0;

    has_TLS10 = pkcs11_has_mechanism(mech_list, CKM_TLS_MASTER_KEY_DERIVE) &&
        pkcs11_has_mechanism(mech_list, CKM_TLS_MASTER_KEY_DERIVE);
    has_TLS12 = pkcs11_has_mechanism(mech_list, CKM_TLS12_MASTER_KEY_DERIVE) &&
        pkcs11_has_mechanism(mech_list, CKM_TLS12_MASTER_KEY_DERIVE) && hashPrf;
    if (hasTLS10 || hasTLS12) {
        rv = acvp_enable_kdf135_tls_cap(ctx, ACVP_KDF135_TLS,
                                                &pkcs11_kdf_tls_handler);
        CHECK_ENABLE_CAP_RV(rv, "TLS KDF");
        rv = acvp_enable_prereq_cap(ctx, ACVP_KDF135_TLS,
                                    ACVP_PREREQ_SHA, value);
        CHECK_ENABLE_CAP_RV(rv, "TLS Prereq SHA");
        rv = acvp_enable_prereq_cap(ctx, ACVP_KDF135_TLS,
                                    ACVP_PREREQ_HMAC, value);
        CHECK_ENABLE_CAP_RV(rv, "TLS Prereq HMAC");
        if (hasTLS12) {
            rv = acvp_enable_kdf135_tls_cap_parm(ctx, ACVP_KDF135_TLS,
                                             ACVP_KDF135_TLS12, hashPrf);
            CHECK_ENABLE_CAP_RV(rv, "TLS 1.2 Hashes");
        }
        if (hasTLS10) {
            rv = acvp_enable_kdf135_tls_cap_parm(ctx, ACVP_KDF135_TLS,
                                             ACVP_KDF135_TLS10_TLS11, 0);
            CHECK_ENABLE_CAP_RV(rv, "TLS 1.[01] Enable");
        }
    }
#endif
}

static void enable_dsa (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list) {
}


static void enable_rsa_set_keysize(ACVP_CTX *ctx, CK_MECHANISM_TYPE *mech_list,
                                   ACVP_RSA_MODE rsa_mode,  int key_size,
                                   ACVP_RSA_SIG_TYPE sig_type, int salt) {
    ACVP_RESULT rv;

    if (pkcs11_has_mechanism(mech_list, CKM_SHA224)) {
        rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA,
                    rsa_mode, sig_type, key_size, ACVP_RSA_SHA_224, salt);
        CHECK_ENABLE_CAP_RV(rv," RSA Op SHA 224");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA256)) {
        rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA,
                    rsa_mode, sig_type, key_size, ACVP_RSA_SHA_256, salt);
        CHECK_ENABLE_CAP_RV(rv," RSA Op SHA 256");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA384)) {
        rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA,
                    rsa_mode, sig_type, key_size, ACVP_RSA_SHA_384, salt);
        CHECK_ENABLE_CAP_RV(rv," RSA Op SHA 384");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA512)) {
        rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA,
                    rsa_mode, sig_type, key_size, ACVP_RSA_SHA_512, salt);
        CHECK_ENABLE_CAP_RV(rv," RSA Op SHA 512");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA512_224)) {
        rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA,
                    rsa_mode, sig_type, key_size, ACVP_RSA_SHA_512_224, salt);
        CHECK_ENABLE_CAP_RV(rv," RSA Op SHA 512/224");
    }
    if (pkcs11_has_mechanism(mech_list, CKM_SHA512_256)) {
        rv = acvp_enable_rsa_cap_sig_type_parm(ctx, ACVP_RSA,
                    rsa_mode, sig_type, key_size, ACVP_RSA_SHA_512_256, salt);
        CHECK_ENABLE_CAP_RV(rv," RSA Op SHA 512/256");
    }
}

static void enable_rsa_op (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list,
                           CK_MECHANISM_TYPE rsa_mech, ACVP_RSA_MODE rsa_mode,
                           ACVP_RSA_SIG_TYPE sigType, int salt) {
    ACVP_RESULT rv;
    CK_MECHANISM_INFO mech_info;

    rv = pkcs11_get_mechanism_info(rsa_mech, &mech_info);
    CHECK_ENABLE_CAP_RV2(rv, "Get RSA Flags %lx", rsa_mech);

    rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_SIGGEN,
                        ACVP_SIG_TYPE, sigType);
    CHECK_ENABLE_CAP_RV(rv, "RSA Op Set Sig Type");
    if (pkcs11_supports_key(&mech_info, 2048, BITS)) {
          enable_rsa_set_keysize(ctx, mech_list, rsa_mode,
                                 MOD_RSA_2048, sigType, salt);
    }
    if (pkcs11_supports_key(&mech_info, 3072, BITS)) {
          enable_rsa_set_keysize(ctx, mech_list, rsa_mode,
                                 MOD_RSA_3072, sigType, salt);
    }
    if (pkcs11_supports_key(&mech_info, 4096, BITS)) {
          enable_rsa_set_keysize(ctx, mech_list, rsa_mode,
                                 MOD_RSA_4096, sigType, salt);
    }
}

CK_MECHANISM_TYPE rsa_key_gen_mech[] = {
  CKM_RSA_PKCS_KEY_PAIR_GEN,
  CKM_RSA_X9_31_KEY_PAIR_GEN,
  CKM_INVALID_MECHANISM };


static void enable_rsa (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list) {
    ACVP_RESULT rv;
    char value[] = "same";
    CK_MECHANISM_TYPE key_gen_mech =
                        pkcs11_get_mechanism(mech_list, rsa_key_gen_mech);
    int key_gen = key_gen_mech != CKM_INVALID_MECHANISM;
    int has_rsa_pkcs = pkcs11_has_mechanism(mech_list, CKM_RSA_PKCS);
    int has_rsa_pss = pkcs11_has_mechanism(mech_list, CKM_RSA_PKCS_PSS);
    int has_rsa_x9_31 = pkcs11_has_mechanism(mech_list, CKM_RSA_X9_31);
    int has_rsa = has_rsa_pkcs || has_rsa_pss || has_rsa_x9_31;
    BIGNUM *expo = BN_new(); /* sigh */
    unsigned long mm = /*RSA_F4; */ 65535;
    CK_MECHANISM_INFO mech_info;

    /* replace with hex string in next version */
    if (!BN_set_word(expo, mm)) {
        printf("Bignum API fail\n");
        exit(1);
    }

    /*
     * Enable RSA keygen...
     */
    if ((key_gen) || (has_rsa)) {
        rv = acvp_enable_rsa_cap(ctx, ACVP_RSA, &pkcs11_rsa_handler);
        CHECK_ENABLE_CAP_RV(rv, "RSA");
        rv = acvp_enable_prereq_cap(ctx, ACVP_RSA, ACVP_PREREQ_SHA, value);
        CHECK_ENABLE_CAP_RV(rv, "RSA Prereq SHA");
        if (has_nss_drbg()) {
            rv = acvp_enable_prereq_cap(ctx, ACVP_RSA,
                                            ACVP_PREREQ_DRBG, value);
            CHECK_ENABLE_CAP_RV(rv, "RSA Prereq DRBG");
        }
        if (key_gen) {
            rv = pkcs11_get_mechanism_info(key_gen_mech, &mech_info);
            CHECK_ENABLE_CAP_RV(rv, "Get RSA_KEY_GEN Flags");
            /* the PKCS #11 API only support generating keys with fixed
             * Exponents and no external seeding. We don't know the underlying
             * system so we mark it B 3.4. Tokens vendors can add vendor
             * specific code to handle other tests if they want */
            rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN,
                                  ACVP_RAND_PQ, RSA_RAND_PQ_B34);
            CHECK_ENABLE_CAP_RV(rv, "RSA KeyGen B3.4");
            rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN,
                                ACVP_PUB_EXP, RSA_PUB_EXP_FIXED);
            CHECK_ENABLE_CAP_RV(rv, "RSA KeyGen Public Exponent Is Fixed");
            rv = acvp_enable_rsa_cap_parm(ctx, ACVP_RSA, ACVP_RSA_MODE_KEYGEN,
                                ACVP_RSA_INFO_GEN_BY_SERVER, 0);
            CHECK_ENABLE_CAP_RV(rv, "RSA KeyGen Gen Info By Server 0");
            rv = acvp_enable_rsa_bignum_parm(ctx, ACVP_RSA,
                        ACVP_RSA_MODE_KEYGEN, ACVP_FIXED_PUB_EXP_VAL, expo);
            CHECK_ENABLE_CAP_RV(rv, "RSA KeyGen Set Fixed Public Exponent");
            if (pkcs11_supports_key(&mech_info, 2048, BITS)) {
                rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA,
                         ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROB_PRIME,
                         MOD_RSA_2048, PRIME_TEST_TBLC2_NAME);
                CHECK_ENABLE_CAP_RV(rv, "RSA KeyGen 2048 TBL C2");
           }
           if (pkcs11_supports_key(&mech_info, 3072, BITS)) {
                rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA,
                         ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROB_PRIME,
                         MOD_RSA_3072, PRIME_TEST_TBLC2_NAME);
                CHECK_ENABLE_CAP_RV(rv, "RSA KeyGen 3072 TBL C2");
            }
            if (pkcs11_supports_key(&mech_info, 4096, BITS)) {
                rv = acvp_enable_rsa_primes_parm(ctx, ACVP_RSA,
                         ACVP_RSA_MODE_KEYGEN, ACVP_CAPS_PROB_PRIME,
                         MOD_RSA_4096, PRIME_TEST_TBLC2_NAME);
                CHECK_ENABLE_CAP_RV(rv, "RSA KeyGen 4096 TBL C2");
            }
        }
        if (has_rsa) {
            CHECK_ENABLE_CAP_RV(rv, "Get RSA Flags");
            if (has_rsa_x9_31) {
                rv = pkcs11_get_mechanism_info(CKM_RSA_X9_31, &mech_info);
                CHECK_ENABLE_CAP_RV(rv, "Get RSA x9_31 Flags");

                if ((mech_info.flags & (CKF_SIGN|CKF_SIGN_RECOVER)) != 0) {
                    enable_rsa_op(ctx, mech_list, ACVP_RSA_MODE_SIGGEN,
                                    CKM_RSA_X9_31, RSA_SIG_TYPE_X931, 0);
                }
                if ((mech_info.flags & (CKF_VERIFY|CKF_VERIFY_RECOVER)) != 0) {
                    enable_rsa_op(ctx, mech_list, ACVP_RSA_MODE_SIGVER,
                                    CKM_RSA_X9_31, RSA_SIG_TYPE_X931, 0);
                }
           }
            if (has_rsa_pss) {
                rv = pkcs11_get_mechanism_info(CKM_RSA_PKCS_PSS, &mech_info);
                CHECK_ENABLE_CAP_RV(rv, "Get RSA PSS Flags");

                if ((mech_info.flags & (CKF_SIGN|CKF_SIGN_RECOVER)) != 0) {
                    enable_rsa_op(ctx, mech_list, ACVP_RSA_MODE_SIGGEN,
                                    CKM_RSA_PKCS_PSS, RSA_SIG_TYPE_PKCS1PSS,
                                    RSA_SALT_SIGGEN_28);
                    /* todo add other salt types */
                }
                if ((mech_info.flags & (CKF_VERIFY|CKF_VERIFY_RECOVER)) != 0) {
                    enable_rsa_op(ctx, mech_list, ACVP_RSA_MODE_SIGVER,
                                    CKM_RSA_PKCS_PSS, RSA_SIG_TYPE_PKCS1PSS,
                                    RSA_SALT_SIGGEN_28);
                    /* todo add other salt types */
                }
           }
           if (has_rsa_pkcs) {
                rv = pkcs11_get_mechanism_info(CKM_RSA_PKCS, &mech_info);
                CHECK_ENABLE_CAP_RV(rv, "Get RSA PKCS 1 Flags");

                if ((mech_info.flags & (CKF_SIGN|CKF_SIGN_RECOVER)) != 0) {
                    enable_rsa_op(ctx, mech_list, ACVP_RSA_MODE_SIGGEN,
                                    CKM_RSA_PKCS, RSA_SIG_TYPE_PKCS1V15, 0);
                }
                if ((mech_info.flags & (CKF_VERIFY|CKF_VERIFY_RECOVER)) != 0) {
                    enable_rsa_op(ctx, mech_list, ACVP_RSA_MODE_SIGVER,
                                    CKM_RSA_PKCS, RSA_SIG_TYPE_PKCS1V15, 0);
                }
            }
        }
    }
}

static void enable_drbg (ACVP_CTX *ctx,  CK_MECHANISM_TYPE *mech_list) {
    ACVP_RESULT rv;
    /*
     * Register DRBG. PKCS #11 does not direct access the the FIPS-180
     * drbg interface. Only set these up if we've found the interface in
     * freebl
     */
    if (has_nss_drbg()) {
        char value[] = "same";
        char value2[] = "123456";
        rv = acvp_enable_drbg_cap(ctx, ACVP_HASHDRBG, app_drbg_handler);
        CHECK_ENABLE_CAP_RV(rv, "DRBG");
        rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
                                    ACVP_DRBG_DER_FUNC_ENABLED, 0);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Enable SHA 256");

        rv = acvp_enable_drbg_prereq_cap(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
            DRBG_SHA, value);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Prereq SHA");
        rv = acvp_enable_drbg_prereq_cap(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
            DRBG_AES, value2);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Prereq AES");

        rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
            ACVP_DRBG_PRED_RESIST_ENABLED, 1);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Predition Resistance Enabled 1");

        rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
            ACVP_DRBG_RESEED_ENABLED, 1);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Reseed Enabled 1");

        rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
            ACVP_DRBG_ENTROPY_LEN, 0);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Entropy Length 0");

        rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
            ACVP_DRBG_NONCE_LEN, 0);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Nonce Length 0");

        rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
            ACVP_DRBG_PERSO_LEN, 0);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Personalization Length 0");

        rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
            ACVP_DRBG_ADD_IN_LEN, 0);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Add In Length 0");

        rv = acvp_enable_drbg_cap_parm(ctx, ACVP_HASHDRBG, ACVP_DRBG_SHA_256,
            ACVP_DRBG_RET_BITS_LEN, 512);
        CHECK_ENABLE_CAP_RV(rv, "DRBG Return Bits 512");
    }
}



static ACVP_RESULT pkcs11_symetric_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_SYM_CIPHER_TC      *tc;
    unsigned int ct_len, pt_len;
    static CK_SESSION_HANDLE session = CK_INVALID_SESSION;
    CK_MECHANISM_INFO mech_info;
    CK_MECHANISM mech = {CKM_INVALID_MECHANISM, NULL, 0};
    CK_KEY_TYPE key_type = 0;
    CK_OBJECT_HANDLE key_handle;
    ACVP_RESULT rv;
    int last = 9999; /* sigh, the library doesn't explicitly
                      * say which iteration is the last, so we just have to
                      * know. It varies depending on algorithm */

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.symmetric;

    /*printf("%s: enter (tc_id=%d) cipher=%d test=%s mct_index=%d\n",
     __FUNCTION__, tc->tc_id, tc->cipher,
     (tc->test_type == ACVP_SYM_TEST_TYPE_MCT) ? "MCT" : "AFT",
     tc->mct_index);  */

    switch (tc->cipher) {
    case ACVP_AES_ECB:
        mech.mechanism = CKM_AES_ECB;
        mech.pParameter = NULL;
        mech.ulParameterLen = 0;
        key_type = CKK_AES;
        last=999;
        break;
    case ACVP_AES_CTR:
        mech.mechanism = CKM_AES_CTR;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_AES;
        last=999;
        break;
    case ACVP_AES_CFB1:
        mech.mechanism = CKM_AES_CFB1;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_AES;
        last=999;
        break;
    case ACVP_AES_CFB8:
        mech.mechanism = CKM_AES_CFB8;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_AES;
        last=999;
        break;
    case ACVP_AES_CFB128:
        mech.mechanism = CKM_AES_CFB128;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_AES;
        last=999;
        break;
    case ACVP_AES_OFB:
        mech.mechanism = CKM_AES_OFB;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_AES;
        last=999;
        break;
    case ACVP_AES_CBC:
        mech.mechanism = CKM_AES_CBC;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_AES;
        last=999;
        break;
    case ACVP_TDES_ECB:
        mech.mechanism = CKM_DES3_ECB;
        mech.pParameter = NULL;
        mech.ulParameterLen = 0;
        key_type = CKK_DES3;
        break;
    case ACVP_TDES_CBC:
        mech.mechanism = CKM_DES3_CBC;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = 8;
        key_type = CKK_DES3;
        break;
    case ACVP_TDES_OFB:
        mech.mechanism = CKM_DES3_OFB8;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_DES3;
    case ACVP_TDES_CFB64:
        mech.mechanism = CKM_DES3_CFB64;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_DES3;
    case ACVP_TDES_CFB8:
        mech.mechanism = CKM_DES3_CFB8;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_DES3;
        break;
    case ACVP_TDES_CFB1:
        mech.mechanism = CKM_DES3_CFB1;
        mech.pParameter = tc->iv;
        mech.ulParameterLen = tc->iv_len;
        key_type = CKK_DES3;
        break;
    default:
        fprintf(stderr,
"Error: Unsupported Crypto mode requested by ACVP server %s(%d)\n",
        acvp_lookup_cipher_name(tc->cipher),tc->cipher);
        return ACVP_NO_CAP;
        break;
    }

    /*
     * validate the key size
     */
    rv = pkcs11_get_mechanism_info(mech.mechanism, &mech_info);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr, "Error: Couldn't get mechanism info for %s, (0x%lx)\n",
                acvp_lookup_cipher_name(tc->cipher), mech.mechanism);
        return rv;
    }
    if (!pkcs11_supports_key(&mech_info, tc->key_len, BYTES)) {
        fprintf(stderr,"Error: Unsupported key length (%d) for %s (0x%lx)\n",
                tc->key_len, acvp_lookup_cipher_name(tc->cipher),
                mech.mechanism);
        return ACVP_NO_CAP;
    }

    if (session == CK_INVALID_SESSION) {
        rv = pkcs11_open_session(slot_id,&session);
        CHECK_CRYPTO_RV(rv, "Couldn't get session");
        key_handle = pkcs11_import_sym_key(session,key_type,
                tc->direction == ACVP_DIR_ENCRYPT? CKA_ENCRYPT : CKA_DECRYPT,
                tc->key,tc->key_len/BYTES);
        if (key_handle == CK_INVALID_HANDLE) {
             pkcs11_close_session(session);
             session = CK_INVALID_SESSION;
            return ACVP_CRYPTO_MODULE_FAIL;
        }
    }

    /* If Monte Carlo we need to be able to init and then update
     * one thousand times before we complete each iteration.
     */
    if (tc->test_type == ACVP_SYM_TEST_TYPE_MCT) {
        pkcs11_final final;
        if (tc->direction == ACVP_DIR_ENCRYPT) {
            if (tc->mct_index == 0) {
                rv = pkcs11_encrypt_init(session, &mech, key_handle);
                CHECK_CRYPTO_RV_CLEANUP(rv, "in C_EncryptInit");
            }
            ct_len = tc->ct_len;
            rv = pkcs11_encrypt_update(session, tc->pt, tc->pt_len,
                                        tc->ct, &ct_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "in C_EncryptUpdate");
            tc->ct_len = ct_len;
            final = pkcs11_encrypt_final;
        } else if (tc->direction == ACVP_DIR_DECRYPT) {
            if (tc->mct_index == 0) {
                rv = pkcs11_decrypt_init(session, &mech, key_handle);
                CHECK_CRYPTO_RV_CLEANUP(rv, "in C_DecryptInit");
            }
            pt_len = tc->pt_len;
            rv = pkcs11_decrypt_update(session, tc->ct, tc->ct_len,
                                        tc->pt, &pt_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "in C_DecryptUpdate");
            tc->pt_len = pt_len;
            final = pkcs11_decrypt_final;
        } else {
            fprintf(stderr,"Unsupported direction %d\n",tc->direction);
            pkcs11_close_session(session);
            session = CK_INVALID_SESSION;
            return ACVP_UNSUPPORTED_OP;
        }
        if (tc->mct_index == last) {
           (*final)(session, NULL, NULL);
           pkcs11_close_session(session);
           session = CK_INVALID_SESSION;
        }
    } else {
        if (tc->direction == ACVP_DIR_ENCRYPT) {
            rv = pkcs11_encrypt_init(session, &mech, key_handle);
            CHECK_CRYPTO_RV_CLEANUP(rv, "in C_EncryptInit");
            ct_len = tc->ct_len;
            rv = pkcs11_encrypt_update(session, tc->pt, tc->pt_len,
                                        tc->ct, &ct_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "in C_EncryptUpdate");
            pkcs11_encrypt_final(session, NULL, NULL);
            tc->ct_len = ct_len;
            /*EVP_EncryptFinal_ex(&cipher_ctx, tc->ct + ct_len, &ct_len);
            tc->ct_len += ct_len; */
        } else if (tc->direction == ACVP_DIR_DECRYPT) {
            rv = pkcs11_decrypt_init(session, &mech, key_handle);
            CHECK_CRYPTO_RV_CLEANUP(rv, "in C_DecryptInit");
            pt_len = tc->pt_len;
            rv = pkcs11_decrypt_update(session, tc->ct, tc->ct_len,
                                        tc->pt, &pt_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "in C_DecryptUpdate");
            tc->pt_len = pt_len;
            pkcs11_decrypt_final(session, NULL, NULL);
            /*EVP_DecryptFinal_ex(&cipher_ctx, tc->pt + pt_len, &pt_len);
            tc->pt_len += pt_len;*/
        } else {
            fprintf(stderr,"Unsupported direction %d\n",tc->direction);
            return ACVP_UNSUPPORTED_OP;
        }
        pkcs11_close_session(session);
        session = CK_INVALID_SESSION;
    }

    return ACVP_SUCCESS;

cleanup:
    pkcs11_close_session(session);
    session = CK_INVALID_SESSION;
    return rv;
}


static ACVP_RESULT pkcs11_keywrap_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_SYM_CIPHER_TC      *tc;
    unsigned int ct_len, pt_len;
    CK_SESSION_HANDLE session = CK_INVALID_SESSION;
    CK_MECHANISM_INFO mech_info;
    CK_MECHANISM mech = {CKM_INVALID_MECHANISM, NULL, 0};
    CK_KEY_TYPE key_type = 0;
    CK_OBJECT_HANDLE key_handle;
    ACVP_RESULT rv;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.symmetric;

    printf("%s: enter (tc_id=%d)\n", __FUNCTION__, tc->tc_id);

    switch (tc->cipher) {
    case ACVP_AES_KW:
        mech.mechanism = CKM_AES_KEY_WRAP;
        key_type = CKK_AES;
        break;
    default:
        fprintf(stderr,
"Error: Unsupported keywrap mode requested by ACVP server %s(%d)\n",
        acvp_lookup_cipher_name(tc->cipher),tc->cipher);
        return ACVP_NO_CAP;
        break;
    }

    /*
     * validate the key size
     */
    rv = pkcs11_get_mechanism_info(mech.mechanism, &mech_info);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr, "Error: Couldn't get mechanism info for %s, (0x%lx)\n",
                acvp_lookup_cipher_name(tc->cipher), mech.mechanism);
        return rv;
    }
    if (!pkcs11_supports_key(&mech_info, tc->key_len, BYTES)) {
        fprintf(stderr,"Error: Unsupported key length (%d) for %s (0x%lx)\n",
                tc->key_len, acvp_lookup_cipher_name(tc->cipher),
                mech.mechanism);
        return ACVP_NO_CAP;
    }

    rv = pkcs11_open_session(slot_id,&session);
    CHECK_CRYPTO_RV(rv, "Couldn't get session");
    key_handle = pkcs11_import_sym_key(session,key_type,
                tc->direction == ACVP_DIR_ENCRYPT? CKA_ENCRYPT : CKA_DECRYPT,
                tc->key,tc->key_len/BYTES);
    if (key_handle == CK_INVALID_HANDLE) {
        pkcs11_close_session(session);
        return ACVP_CRYPTO_MODULE_FAIL;
    }


    if (tc->direction == ACVP_DIR_ENCRYPT) {
        rv = pkcs11_encrypt_init(session, &mech, key_handle);
        CHECK_CRYPTO_RV_CLEANUP(rv, "in C_EncryptInit (Keywrapping)");
        ct_len = tc->ct_len;
        rv = pkcs11_encrypt(session, tc->pt, tc->pt_len, tc->ct, &ct_len);
        CHECK_CRYPTO_RV_CLEANUP(rv, "in C_Encrypt (Keywrapping)");
        tc->ct_len = ct_len;
    } else if (tc->direction == ACVP_DIR_DECRYPT) {
        rv = pkcs11_decrypt_init(session, &mech, key_handle);
        CHECK_CRYPTO_RV_CLEANUP(rv, "in C_DecryptInit (Keywrapping)");
        pt_len = tc->pt_len;
        rv = pkcs11_decrypt(session, tc->ct, tc->ct_len, tc->pt, &pt_len);
        CHECK_CRYPTO_RV_CLEANUP(rv, "in C_Decrypt (Keywrapping)");
        tc->pt_len = pt_len;
    } else {
        fprintf(stderr,"Unsupported direction %d\n",tc->direction);
        return ACVP_UNSUPPORTED_OP;
    }

    rv = ACVP_SUCCESS;
cleanup:
    pkcs11_close_session(session);
    session = CK_INVALID_SESSION;
    return rv;
}

/*
 * This fuction is invoked by libacvp when an AES crypto
 * operation is needed from the crypto module being
 * validated.  This is a callback provided to libacvp when
 * acvp_enable_capability() is invoked to register the
 * AES-GCM capabilitiy with libacvp.  libacvp will in turn
 * invoke this function when it needs to process an AES-GCM
 * test case.
 */
//TODO: I have mixed feelings on returing ACVP_RESULT.  This is
//      application layer code outside of libacvp.  Should we
//      return a simple pass/fail?  Should we provide a separate
//      enum that applications can use?
static ACVP_RESULT pkcs11_aead_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_SYM_CIPHER_TC      *tc;
    unsigned int pt_len;
    CK_SESSION_HANDLE session = CK_INVALID_SESSION;
    CK_MECHANISM_INFO mech_info;
    CK_MECHANISM mech = {CKM_INVALID_MECHANISM, NULL, 0};
    CK_KEY_TYPE key_type = 0;
    CK_OBJECT_HANDLE key_handle;
    CK_GCM_PARAMS gcm_params;
    CK_CCM_PARAMS ccm_params;
    unsigned char *tbuf;
    unsigned int tbuf_len;
    ACVP_RESULT rv;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    /* for now, just use the single part PKCS #11 interface of v2.0 */

    tc = test_case->tc.symmetric;

    /* printf("%s: enter (tc_id=%d)\n", __FUNCTION__, tc->tc_id); */

    if (tc->direction != ACVP_DIR_ENCRYPT && tc->direction != ACVP_DIR_DECRYPT) {
        fprintf(stderr,"Unsupported direction %d\n",tc->direction);
        return ACVP_UNSUPPORTED_OP;
    }


    /* todo: add PKCS #11 v3.0 interface */
    switch (tc->cipher) {
    case ACVP_AES_GCM:
        mech.mechanism = CKM_AES_GCM;
        mech.pParameter = &gcm_params;
        mech.ulParameterLen = sizeof(gcm_params);
        key_type = CKK_AES;
        gcm_params.pIv = tc->iv;
        gcm_params.ulIvLen = tc->iv_len;
        gcm_params.pAAD = tc->aad;
        gcm_params.ulAADLen = tc->aad_len;
        gcm_params.ulTagBits = tc->tag_len*8;
        break;
    case ACVP_AES_CCM:
        mech.mechanism = CKM_AES_CCM;
        mech.pParameter = &ccm_params;
        mech.ulParameterLen = sizeof(ccm_params);
        key_type = CKK_AES;
        ccm_params.pNonce = tc->iv;
        ccm_params.ulNonceLen = tc->iv_len;
        ccm_params.pAAD = tc->aad;
        ccm_params.ulAADLen = tc->aad_len;
        ccm_params.ulMACLen = tc->tag_len;
        break;

    default:
        fprintf(stderr,
"Error: Unsupported AEAD mode requested by ACVP server %s(%d)\n",
        acvp_lookup_cipher_name(tc->cipher),tc->cipher);
        return ACVP_NO_CAP;
        break;
    }

    rv = pkcs11_get_mechanism_info(mech.mechanism, &mech_info);
    if (rv != ACVP_SUCCESS) {
        fprintf(stderr, "Error: Couldn't get mechanism info for %s, (0x%lx)\n",
                acvp_lookup_cipher_name(tc->cipher), mech.mechanism);
        return rv;
    }
    if (!pkcs11_supports_key(&mech_info, tc->key_len, BYTES)) {
        fprintf(stderr,"Error: Unsupported key length (%d) for %s (0x%lx)\n",
                tc->key_len, acvp_lookup_cipher_name(tc->cipher), mech.mechanism);
        return ACVP_NO_CAP;
    }

    rv = pkcs11_open_session(slot_id,&session);
    CHECK_CRYPTO_RV(rv, "Couldn't get session");
    key_handle = pkcs11_import_sym_key(session,key_type,
                tc->direction == ACVP_DIR_ENCRYPT? CKA_ENCRYPT : CKA_DECRYPT,
                tc->key,tc->key_len/BYTES);
    if (key_handle == CK_INVALID_HANDLE) {
        return ACVP_CRYPTO_MODULE_FAIL;
    }

    tbuf_len = tc->ct_len + tc->tag_len;
    tbuf = malloc(tbuf_len);
    if (tc->direction == ACVP_DIR_ENCRYPT) {
        rv = pkcs11_encrypt_init(session, &mech, key_handle);
        CHECK_CRYPTO_RV_CLEANUP(rv, "in C_EncryptInit (AEAD)");
        rv = pkcs11_encrypt(session, tc->pt, tc->pt_len, tbuf, &tbuf_len);
        CHECK_CRYPTO_RV_CLEANUP(rv, "in C_Encrypt (AEAD)");
        if (tbuf_len < tc->tag_len) {
            fprintf(stderr,"AEAD failed to generate Tag\n");
            free(tbuf);
            pkcs11_close_session(session);
            return ACVP_CRYPTO_MODULE_FAIL;
        }
        tc->ct_len = tbuf_len - tc->tag_len;
        memcpy(tc->ct, tbuf, tc->ct_len);
        memcpy(tc->tag, tbuf+tc->ct_len, tc->tag_len);
     } else if (tc->direction == ACVP_DIR_DECRYPT) {
        memcpy(tbuf, tc->ct, tc->ct_len);
        memcpy(tbuf+tc->ct_len, tc->tag, tc->tag_len);
        rv = pkcs11_decrypt_init(session, &mech, key_handle);
        CHECK_CRYPTO_RV_CLEANUP(rv, "in C_DecryptInit (AEAD)");
        pt_len = tbuf_len; /* sigh */
        rv = pkcs11_decrypt(session, tbuf, tbuf_len, tc->pt, &pt_len);
        CHECK_CRYPTO_RV_EXPECTED_CLEANUP(rv, ACVP_CRYPTO_TAG_FAIL,
                                        "in C_Decrypt (AEAD)");
        tc->pt_len = pt_len;
    }
    rv = ACVP_SUCCESS;
cleanup:
    free(tbuf);
    pkcs11_close_session(session);
    session = CK_INVALID_SESSION;

    return rv;
}

static ACVP_RESULT pkcs11_digest_handler(ACVP_TEST_CASE *test_case)
{
    ACVP_HASH_TC        *tc;
    CK_SESSION_HANDLE session;
    CK_MECHANISM mech = {CKM_INVALID_MECHANISM, NULL, 0};
    ACVP_RESULT rv;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.hash;

    /*printf("%s: enter (tc_id=%d)\n", __FUNCTION__, tc->tc_id); */

    switch (tc->cipher) {
    case ACVP_SHA1:
        mech.mechanism = CKM_SHA_1;
        break;
    case ACVP_SHA224:
        mech.mechanism = CKM_SHA224;
        break;
    case ACVP_SHA256:
        mech.mechanism = CKM_SHA256;
        break;
    case ACVP_SHA384:
        mech.mechanism = CKM_SHA384;
        break;
    case ACVP_SHA512:
        mech.mechanism = CKM_SHA512;
        break;
    default:
        fprintf(stderr,
"Error: Unsupported hash algorithm requested by ACVP server %s(%d)\n",
        acvp_lookup_cipher_name(tc->cipher),tc->cipher);
        return ACVP_NO_CAP;
        break;
    }

    rv = pkcs11_open_session(slot_id,&session);
    CHECK_CRYPTO_RV(rv, "Couldn't get session");

    /* If Monte Carlo we need to be able to init and then update
     * one thousand times before we complete each iteration.
     * <Q: what does 'complete each iteration' mean? I'm following
     * app_main, which finishes each digest after 3 update messages>
     */
    tc->md_len = 64; /* max hash length */
    if (tc->test_type == ACVP_HASH_TEST_TYPE_MCT) {
        rv = pkcs11_digest_init(session, &mech);
            CHECK_CRYPTO_RV_CLEANUP(rv, "C_DigestInit failed\n");
        rv = pkcs11_digest_update(session, tc->m1, tc->msg_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "C_DigestUpdate failed\n");
        rv = pkcs11_digest_update(session, tc->m2, tc->msg_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "C_DigestUpdate failed\n");
        rv = pkcs11_digest_update(session, tc->m3, tc->msg_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "C_DigestUpdate failed\n");
        rv = pkcs11_digest_final(session, tc->md, &tc->md_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "C_DigestFinal failed\n");

    } else {
        rv = pkcs11_digest_init(session, &mech);
            CHECK_CRYPTO_RV_CLEANUP(rv, "C_DigestInit failed\n");
        rv = pkcs11_digest_update(session, tc->msg, tc->msg_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "C_DigestUpdate failed\n");
        rv = pkcs11_digest_final(session, tc->md, &tc->md_len);
            CHECK_CRYPTO_RV_CLEANUP(rv, "C_DigestFinal failed\n");
    }
    rv = ACVP_SUCCESS;
cleanup:
    pkcs11_close_session(session);

    return rv;
}

static ACVP_RESULT pkcs11_mac_handler(ACVP_TEST_CASE *test_case) {
    ACVP_HMAC_TC    *tc;
    static CK_SESSION_HANDLE session = CK_INVALID_SESSION;
    CK_MECHANISM mech = {CKM_INVALID_MECHANISM, NULL, 0};
    CK_KEY_TYPE key_type = 0;
    CK_OBJECT_HANDLE key_handle;
    ACVP_RESULT rv;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.hmac;

    switch (tc->cipher) {
    case ACVP_HMAC_SHA1:
      mech.mechanism = CKM_SHA_1_HMAC;
      mech.pParameter = NULL;
      mech.ulParameterLen = 0;
      key_type = CKK_SHA_1_HMAC;
      break;
    case ACVP_HMAC_SHA2_224:
      mech.mechanism = CKM_SHA224_HMAC;
      mech.pParameter = NULL;
      mech.ulParameterLen = 0;
      key_type = CKK_SHA224_HMAC;
      break;
    case ACVP_HMAC_SHA2_256:
      mech.mechanism = CKM_SHA256_HMAC;
      mech.pParameter = NULL;
      mech.ulParameterLen = 0;
      key_type = CKK_SHA256_HMAC;
      break;
    case ACVP_HMAC_SHA2_384:
      mech.mechanism = CKM_SHA384_HMAC;
      mech.pParameter = NULL;
      mech.ulParameterLen = 0;
      key_type = CKK_SHA384_HMAC;
      break;
    case ACVP_HMAC_SHA2_512:
      mech.mechanism = CKM_SHA512_HMAC;
      mech.pParameter = NULL;
      mech.ulParameterLen = 0;
      key_type = CKK_SHA512_HMAC;
      break;
#ifdef notdef
    case ACVP_CMAC_AES:
      mech.mechanism = CKM_AES_CMAC;
      mech.pParameter = NULL;
      mech.ulParameterLen = 0;
      key_type = CKK_GENERIC_SECRET;
      break;
#endif
    case ACVP_CMAC_TDES:
      mech.mechanism = CKM_DES3_CMAC;
      mech.pParameter = NULL;
      mech.ulParameterLen = 0;
      key_type = CKK_GENERIC_SECRET;
      break;
    default:
        printf("Error: Unsupported hash algorithm requested by ACVP server\n");
        return ACVP_NO_CAP;
        break;
    }
    rv = pkcs11_open_session(slot_id,&session);
    CHECK_CRYPTO_RV(rv, "Couldn't get session");

    key_handle = pkcs11_import_sym_key(session, key_type, CKA_SIGN,
                tc->key,tc->key_len);
    if (key_handle == CK_INVALID_HANDLE) {
        /* try it one more time with CKK_GENERIC_SECRET */
        key_handle = pkcs11_import_sym_key(session, CKK_GENERIC_SECRET,
                CKA_SIGN, tc->key,tc->key_len);
        if (key_handle == CK_INVALID_HANDLE) {
            pkcs11_close_session(session);
            session = CK_INVALID_SESSION;
            return ACVP_CRYPTO_MODULE_FAIL;
        }
    }

    rv = pkcs11_sign_init(session, &mech, key_handle);
    CHECK_CRYPTO_RV_CLEANUP(rv, "MAC C_SignInit failed\n");

    rv = pkcs11_sign_update(session, tc->msg, tc->msg_len);
    CHECK_CRYPTO_RV_CLEANUP(rv, "MAC C_SignUpdate failed\n");

    rv = pkcs11_sign_final(session, tc->mac, &tc->mac_len);
    CHECK_CRYPTO_RV_CLEANUP(rv, "HMAC C_SignFinal failed\n");

    rv = ACVP_SUCCESS;
cleanup:
    pkcs11_close_session(session);

    return ACVP_SUCCESS;
}

static ACVP_RESULT app_drbg_handler(ACVP_TEST_CASE *test_case)
{
/* There isn't a PKCS #11 interface for drbg tests, Use the private NSS interface for freebl */
#ifndef ENABLE_NSS_DRBG
    return ACVP_UNSUPPORTED_OP;
#else
    ACVP_RESULT     result = ACVP_SUCCESS;
    ACVP_DRBG_TC    *tc;
    SECStatus rv;

    if (!test_case) {
        return ACVP_INVALID_ARG;
    }

    tc = test_case->tc.drbg;
    /*
     * Init entropy length
     */
    printf("%s: enter (tc_id=%d)\n", __FUNCTION__, tc->tc_id);

    switch(tc->cipher) {
    case ACVP_HASHDRBG:
        switch(tc->mode) {
        case ACVP_DRBG_SHA_256:
            break;
        case ACVP_DRBG_SHA_1:
        case ACVP_DRBG_SHA_384:
        case ACVP_DRBG_SHA_512:
        case ACVP_DRBG_SHA_224:
        case ACVP_DRBG_SHA_512_224:
        case ACVP_DRBG_SHA_512_256:
        default:
            result = ACVP_UNSUPPORTED_OP;
            fprintf(stderr,
"Error: Unsupported drbg algorithm mode requested by ACVP server %s(%d/%d)\n",
        acvp_lookup_cipher_name(tc->cipher),tc->cipher,tc->mode);
            return (result);
            break;
        }
        break;

    case ACVP_HMACDRBG:
    case ACVP_CTRDRBG:
    default:
        result = ACVP_UNSUPPORTED_OP;
       fprintf(stderr,
"Error: Unsupported drbg algorithm requested by ACVP server %s(%d)\n",
        acvp_lookup_cipher_name(tc->cipher),tc->cipher);
        return (result);
        break;
    }

    rv = freebl_drbg_instantiate(tc->entropy, tc->entropy_len/8,
                tc->nonce, tc->nonce_len/8,
                tc->perso_string, tc->perso_string_len/8);
    if (rv != SECSuccess) {
        progress("ERROR: failed to instantiate DRBG");
        result = ACVP_CRYPTO_MODULE_FAIL;
        goto end;
    }

    /*
     * Process predictive resistance flag
     */
    if (tc->pred_resist_enabled) {
        rv = freebl_drbg_reseed(tc->entropy_input_pr, tc->entropy_len/8,
                         tc->additional_input, tc->additional_input_len/8);
        if (rv != SECSuccess) {
            progress("ERROR: failed to reseed drb");
            result = ACVP_CRYPTO_MODULE_FAIL;
            goto end;
        }

        rv = freebl_drbg_generate(tc->drb, tc->drb_len/8, NULL, 0);
        if (rv != SECSuccess) {
            progress("ERROR: failed to generate drb");
            result = ACVP_CRYPTO_MODULE_FAIL;
            goto end;
        }

        rv = freebl_drbg_reseed(tc->entropy_input_pr_1, tc->entropy_len/8,
                         tc->additional_input, tc->additional_input_len/8);
        if (rv != SECSuccess) {
            progress("ERROR: failed to reseed drb");
            result = ACVP_CRYPTO_MODULE_FAIL;
            goto end;
        }

        rv = freebl_drbg_generate(tc->drb, tc->drb_len/8, NULL, 0);
        if (rv != SECSuccess) {
            progress("ERROR: failed to generate drb");
            result = ACVP_CRYPTO_MODULE_FAIL;
            goto end;
        }


    } else {
        rv = freebl_drbg_generate(tc->drb, tc->drb_len/8,
                              tc->additional_input,tc->additional_input_len/8);
        if (rv != SECSuccess) {
            progress("ERROR: failed to generate drb");
            result = ACVP_CRYPTO_MODULE_FAIL;
            goto end;
        }
    }

end:
    freebl_drbg_uninstantiate();
    return result;
#endif
}

static ACVP_RESULT pkcs11_rsa_handler(ACVP_TEST_CASE *test_case) {
    return ACVP_UNSUPPORTED_OP;
}

#ifdef IMPLEMENT
static ACVP_RESULT pkcs11_kdf_tls_handler(ACVP_TEST_CASE *test_case) {
    ACVP_KDF135_TLS_TC    *tc;
    CK_MECHANISM master_mech = { CKM_TLS_MASTER_KEY_DERIVE, NULL, 0 };
    CK_MECHANISM key_block_mech = { CKM_TLS_KEY_AND_MAC_DERIVE, NULL, 0 };
    CK_TLS12_MASTER_KEY_DERIVE_PARAMS tls12_master_params;
    CK_TLS12_KEY_MAT_PARAMS tls12_key_block_params;
    CK_SSL3_MASTER_KEY_DERIVE_PARAMS master_params;
    CK_SSL3_KEY_MAT_PARAMS key_block_params;
    CK_SSL3_KEY_MAT_OUT key_material;
    CKM_MECHANISM_TYPE prf_mech = CKM_SHA_1;
    static CK_OBJECT_CLASS ck_secret = CKO_SECRET_KEY;
    static CK_KEY_TYPE ck_generic = CKK_GENERIC_SECRET;
    static CK_BBOOL ck_true = CK_TRUE;
    static CK_ULONG one = 1;
    CK_ATTRIBUTE derive_template[] = {
        { CKA_CLASS, &ck_secret, sizeof(ck_secret) },
        { CKA_KEY_TYPE, &ck_generic, sizeof(ck_generic) },
        { CKA_DERIVE, &ck_true, sizeof(ck_true) },
        { CKA_VALUE_LEN, &one, sizeof(one) },
    };
    CK_ULONG derive_template_count =
        sizeof(derive_template) / sizeof(derive_template[0]);
    unsigned int keySize;

    tc = test_case->tc.kdf135_tls;

    switch (tc->md)
    {
    case ACVP_KDF135_TLS_CAP_SHA256:
        prf_mech = CKM_SHA256;
        break;
    case ACVP_KDF135_TLS_CAP_SHA384:
        prf_mech = CKM_SHA384;
        break;
    case ACVP_KDF135_TLS_CAP_SHA512:
        prf_mech = CKM_SHA512;
        break;
    default:
        break;
    }

    keySize = tc->kb_len/2;

    switch (tc->method) {
    case ACVP_KDF135_TLS12:
        tls12_master_params.pVersion = NULL;
        tls12_master_params.pCientRandom  = tc->ch_rnd;
        tls12_master_params.pServerRandom = tc->sh_rnd;
        tls12_master_params.pClientRandomLen = SSL3_RANDOM_LENGTH;
        tls12_master_params.pServerRandomLen = SSL3_RANDOM_LENGTH;
        tls12_master_params.prfHashMechanism = prf_mech;
        master_mech.mech = CKM_TLS12_MASTER_KEY_DERIVE;
        master_mech.pParameter = (void *)&tls12_master_params;
        master_mech.ulParameterlen = sizeof(tls12_master_params);
        tls12_key_block_params.ulMacSizeInBits = 0;
        tls12_key_block_params.ulKeySizeInBits = keySize*8;
        tls12_key_block_params.ulIVSizeInBits = 0;
        tls12_key_block_params.pCientRandom  = tc->ch_rnd;
        tls12_key_block_params.pServerRandom = tc->sh_rnd;
        tls12_key_block_params.pClientRandomLen = SSL3_RANDOM_LENGTH;
        tls12_key_block_params.pServerRandomLen = SSL3_RANDOM_LENGTH;
        tls12_key_block_params.pReturnedKeymaterial = &key_material;
        tls12_key_block_params.prfHashMechanism = prf_mech;
        tls12_key_block_params.bIsExport = PR_FALSE;
        key_block_mech.mechanism = CKM_TLS12_KEY_AND_MAC_DERIVE;
        key_block_mech.pParameter = (void *)&tls12_key_block_params;
        key_block_mech.ulParameterLen = sizeof(tls12_key_block_params);
        break
    case ACVP_KDF135_TLS10_11:
        master_params.pVersion = NULL;
        master_params.pCientRandom  = tc->ch_rnd;
        master_params.pServerRandom = tc->sh_rnd;
        master_params.pClientRandomLen = SSL3_RANDOM_LENGTH;
        master_params.pServerRandomLen = SSL3_RANDOM_LENGTH;
        master_mech.pParameter = (void *)&master_params;
        master_mech.ulParameterlen = sizeof(master_params);
        key_block_params.ulMacSizeInBits = 0;
        key_block_params.ulKeySizeInBits = keySize*8;
        key_block_params.ulIVSizeInBits = 0;
        key_block_params.pCientRandom  = tc->ch_rnd;
        key_block_params.pServerRandom = tc->sh_rnd;
        key_block_params.pClientRandomLen = SSL3_RANDOM_LENGTH;
        key_block_params.pServerRandomLen = SSL3_RANDOM_LENGTH;
        key_block_params.pReturnedKeymaterial = &key_material;
        key_block_params.bIsExport = PR_FALSE;
        key_block_mech.pParameter = (void *)&key_block_params;
        key_block_mech.ulParameterLen = sizeof(key_block_params);
        break;
     default:
        printf("\nCrypto module error, Bad TLS type\n");
        return ACVP_CRYPTO_MODULE_FAIL;
    }

    rv = pkcs11_open_session(slot_id,&session);
    CHECK_CRYPTO_RV(rv, "Couldn't get session");

    pms_key_handle = pkcs11_import_sym_key(session, CKK_GENERIC_SECRET,
                CKA_DERIVE, tc->pm,tc->pm_len);
    if (pms_key_handle == CK_INVALID_HANDLE) {
        pkcs11_close_session(session);
        session = CK_INVALID_SESSION;
        return ACVP_CRYPTO_MODULE_FAIL;
    }

    rv = pkcs11_derive_key(session, &master_mech, pms_key_handle,
        derive_template, derive_template_count -1, &master_key_handle);
    CHECK_CRYPTO_RV(rv, "Couldn't derive master secret");

    tmp_len = tc->pm_len;
    rv = pkcs11_get_attribute(session, master_key_handle,
                              tc->msecret1, &tmp_len);
    CHECK_CRYPTO_RV(rv, "Couldn't extract master key");

    rv = pkcs11_derive_key(session, &key_block_mech, master_key_handle,
        derive_template, derive_template_count, &fake_handle);
    CHECK_CRYPTO_RV(rv, "Couldn't derive session keys");

    tmp_len = keySize;
    rv = pkcs11_get_attribute(session, key_material.hClientKey,
                              tc->kblock1, &tmp_len);
    CHECK_CRYPTO_RV(rv, "Couldn't extract key clientKey");
    tmp_len = keySize;
    rv = pkcs11_get_attribute(session, key_material.hServerKey,
                              tc->kblock1[keySize], &tmp_len);
    CHECK_CRYPTO_RV(rv, "Couldn't extract key serverKey");

    return ACVP_SUCCESS;
}
static ACVP_RESULT pkcs11_dsa_handler(ACVP_TEST_CASE *test_case) {
    return ACVP_UNSUPPORTED_OP;
}
#endif
