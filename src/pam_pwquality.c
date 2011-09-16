/*
 * PAM module for password quality checking using libpwquality
 *
 * See the end of the file for Copyright and License Information
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <limits.h>
#include <syslog.h>
#include <libintl.h>
#include "pwquality.h"

/* For Translators: "%s%s" could be replaced with "<service> " or "". */
#define PROMPT1 _("New %s%spassword: ")
/* For Translators: "%s%s" could be replaced with "<service> " or "". */
#define PROMPT2 _("Retype new %s%spassword: ")
#define MISTYPED_PASS _("Sorry, passwords do not match.")

/*
 * here, we make a definition for the externally accessible function
 * in this file (this definition is required for static a module
 * but strongly encouraged generally) it is used to instruct the
 * modules include file to define the function prototypes.
 */

#define PAM_SM_PASSWORD

#include <security/pam_modules.h>
#include <security/_pam_macros.h>
#include <security/pam_ext.h>

/* argument parsing */
#define PAM_DEBUG_ARG       0x0001

struct module_options {
        int retry_times;
        pwquality_settings_t *pwq;
};

#define CO_RETRY_TIMES  1

static int
_pam_parse (pam_handle_t *pamh, struct module_options *opt,
            int argc, const char **argv)
{
        int ctrl = 0;
        pwquality_settings_t *pwq;

        pwq = pwquality_default_settings();
        if (pwq == NULL)
                return -1;

        /* just log error here */
        if (pwquality_read_config(pwq, NULL))
                pam_syslog(pamh, LOG_ERR,
                        "Reading pwquality configuration file failed: %m");

        /* step through arguments */
        for (ctrl = 0; argc-- > 0; ++argv) {
                char *ep = NULL;

                if (!strcmp(*argv, "debug"))
                        ctrl |= PAM_DEBUG_ARG;
                else if (!strncmp(*argv, "type=", 5))
                        pam_set_item (pamh, PAM_AUTHTOK_TYPE, *argv+5);
                else if (!strncmp(*argv, "retry=", 6)) {
                        opt->retry_times = strtol(*argv+6, &ep, 10);
                        if (!ep || (opt->retry_times < 1))
                                opt->retry_times = CO_RETRY_TIMES;
                } else if (!strncmp(*argv, "reject_username", 15)) {
                        /* ignored for compatibility with pam_cracklib */
                } else if (!strncmp(*argv, "authtok_type", 12)) {
                        /* for pam_get_authtok, ignore */;
                } else if (!strncmp(*argv, "use_authtok", 11)) {
                        /* for pam_get_authtok, ignore */;
                } else if (!strncmp(*argv, "use_first_pass", 14)) {
                        /* for pam_get_authtok, ignore */;
                } else if (!strncmp(*argv, "try_first_pass", 14)) {
                        /* for pam_get_authtok, ignore */;
                } else if (pwquality_set_option(pwq, *argv)) {
                        pam_syslog(pamh, LOG_ERR, 
                                "pam_parse: unknown or broken option; %s", *argv);
                }
         }

         opt->pwq = pwq;

         return ctrl;
}

static const char *
make_error_message(int rv, const char *crack_msg)
{
        switch(rv) {
        case PWQ_ERROR_MEM_ALLOC:
                return _("memory allocation error");
        case PWQ_ERROR_SAME_PASSWORD:
                return _("is the same as the old one");
        case PWQ_ERROR_PALINDROME:
                return _("is a palindrome");
        case PWQ_ERROR_CASE_CHANGES_ONLY:
                return _("case changes only");
        case PWQ_ERROR_TOO_SIMILAR:
                return _("is too similar to the old one");
        case PWQ_ERROR_MIN_DIGITS:
        case PWQ_ERROR_MIN_UPPERS:
        case PWQ_ERROR_MIN_LOWERS:
        case PWQ_ERROR_MIN_OTHERS:
        case PWQ_ERROR_MIN_LENGTH:
                return _("is too simple");
        case PWQ_ERROR_ROTATED:
                return _("is rotated");
        case PWQ_ERROR_MIN_CLASSES:
                return _("not enough character classes");
        case PWQ_ERROR_MAX_CONSECUTIVE:
                return _("contains too many same characters consecutively");
        case PWQ_ERROR_EMPTY_PASSWORD:
                return _("No password supplied");
        case PWQ_ERROR_CRACKLIB_CHECK:
                return crack_msg;
        default:
                return _("Error in service module");
        }
}

PAM_EXTERN int
pam_sm_chauthtok(pam_handle_t *pamh, int flags,
                 int argc, const char **argv)
{
        int ctrl;
        struct module_options options;

        memset(&options, 0, sizeof(options));
        options.retry_times = CO_RETRY_TIMES;

        ctrl = _pam_parse(pamh, &options, argc, argv);
        if (ctrl < 0)
                return PAM_BUF_ERR;

        if (flags & PAM_PRELIM_CHECK) {
                /* Check for passwd dictionary
                 * We cannot do that, since the original path is compiled
                 * into the cracklib library and we don't know it.
                 */
                return PAM_SUCCESS;
        } else if (flags & PAM_UPDATE_AUTHTOK) {
                int retval;
                const void *oldtoken;
                int tries;

                retval = pam_get_item(pamh, PAM_OLDAUTHTOK, &oldtoken);
                if (retval != PAM_SUCCESS) {
                        if (ctrl & PAM_DEBUG_ARG)
                                pam_syslog(pamh, LOG_ERR, "Can not get old passwd");
                        oldtoken = NULL;
                }

                tries = 0;
                while (tries < options.retry_times) {
                        const char *crack_msg;
                        const char *newtoken = NULL;

                        tries++;

                        /* Planned modus operandi:
                         * Get a passwd.
                         * Verify it against libpwquality.
                         * If okay get it a second time.
                         * Check to be the same with the first one.
                         * set PAM_AUTHTOK and return
                         */

                        retval = pam_get_authtok_noverify(pamh, &newtoken, NULL);
                        if (retval != PAM_SUCCESS) {
                                pam_syslog(pamh, LOG_ERR, "pam_get_authtok_noverify returned error: %s",
                                        pam_strerror(pamh, retval));
                                continue;
                        } else if (newtoken == NULL) { /* user aborted password change, quit */
                                return PAM_AUTHTOK_ERR;
                        }

                        /* now test this passwd against libpwquality */
                        retval = pwquality_check(options.pwq, newtoken, oldtoken, &crack_msg);

                        if (retval < 0) {
                                const char *msg;
                                msg = make_error_message(retval, crack_msg);
                                if (ctrl & PAM_DEBUG_ARG)
                                        pam_syslog(pamh, LOG_DEBUG, "bad password: %s", msg);
                                pam_error(pamh, _("BAD PASSWORD: %s"), msg);

                                if (getuid() || (flags & PAM_CHANGE_EXPIRED_AUTHTOK)) {
                                        pam_set_item(pamh, PAM_AUTHTOK, NULL);
                                        retval = PAM_AUTHTOK_ERR;
                                        continue;
                                }
                        } else {
                                if (ctrl & PAM_DEBUG_ARG)
                                        pam_syslog(pamh, LOG_DEBUG, "password score: %d", retval);
                        }

                        retval = pam_get_authtok_verify(pamh, &newtoken, NULL);
                        if (retval != PAM_SUCCESS) {
                                pam_syslog(pamh, LOG_ERR, "pam_get_authtok_verify returned error: %s",
                                pam_strerror(pamh, retval));
                                pam_set_item(pamh, PAM_AUTHTOK, NULL);
                                continue;
                        } else if (newtoken == NULL) {      /* user aborted password change, quit */
                                return PAM_AUTHTOK_ERR;
                        }

                        return PAM_SUCCESS;
                }

                pam_set_item (pamh, PAM_AUTHTOK, NULL);

                /* if we have only one try, we can use the real reason,
                 * else say that there were too many tries. */
                if (options.retry_times > 1)
                        return PAM_MAXTRIES;
                else
                        return retval;
        } else {
                if (ctrl & PAM_DEBUG_ARG)
                        pam_syslog(pamh, LOG_NOTICE, "UNKNOWN flags setting %02X",flags);
        }

        return PAM_SERVICE_ERR;
}



#ifdef PAM_STATIC
/* static module data */
struct pam_module _pam_pwquality_modstruct = {
     "pam_pwquality",
     NULL,
     NULL,
     NULL,
     NULL,
     NULL,
     pam_sm_chauthtok
};
#endif

/*
 * Copyright (c) Cristian Gafton <gafton@redhat.com>, 1996.
 *                                              All rights reserved
 * Copyright (c) Red Hat, Inc, 2011
 * Copyright (c) Tomas Mraz <tm@t8m.info>, 2011
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED `AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The following copyright was appended for the long password support
 * added with the libpam 0.58 release:
 *
 * Modificaton Copyright (c) Philip W. Dalrymple III <pwd@mdtsoft.com>
 *       1997. All rights reserved
 *
 * THE MODIFICATION THAT PROVIDES SUPPORT FOR LONG PASSWORD TYPE CHECKING TO
 * THIS SOFTWARE IS PROVIDED `AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
