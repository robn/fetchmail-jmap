/*
 * jmap.c -- JMAP protocol methods
 */

#include  "config.h"

#ifdef JMAP_ENABLE
#include  <stdio.h>
#if defined(STDC_HEADERS)
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include  "fetchmail.h"
#include  "socket.h"
#include  "i18n.h"
#include  "buf.h"

#include <curl/curl.h>
#include <wjelement.h>

static struct buf body_out = BUF_INITIALIZER;
static struct buf body_in = BUF_INITIALIZER;

static CURL *curl_jmap = 0, *curl_blob = 0;
static struct curl_slist *curl_jmap_headers = 0, *curl_blob_headers = 0;

static size_t _max(size_t a, size_t b) {
    return a > b ? a : b;
}

static size_t _jmap_curl_read(char *buf, size_t size, size_t nmemb, void *data)
{
    size_t amount = _max(size*nmemb, body_out.len);
    memcpy(buf, body_out.s, amount);
    buf_remove(&body_out, 0, amount);
    return amount;
}

static size_t _jmap_curl_write(char *buf, size_t size, size_t nmemb, void *data)
{
    size_t amount = size*nmemb;
    buf_appendmap(&body_in, buf, size*nmemb);
    return amount;
}

static int jmap_ok (int sock, char *argbuf) {
    report(stderr, "in jmap_ok()\n");
    return PS_SUCCESS;
}

static size_t _jmap_wjw_callback(char *buf, size_t len, void *data)
{
    buf_appendmap(&body_out, buf, len);
    return len;
}

static int _jmap_call(WJElement batch, WJElement *rbatch)
{
    if (!curl_jmap) {
	curl_jmap = curl_easy_init();
	//curl_easy_setopt(curl_jmap, CURLOPT_VERBOSE, 1);

	curl_easy_setopt(curl_jmap, CURLOPT_POST, 1);

	curl_easy_setopt(curl_jmap, CURLOPT_READFUNCTION, _jmap_curl_read);
	curl_easy_setopt(curl_jmap, CURLOPT_WRITEFUNCTION, _jmap_curl_write);

	curl_jmap_headers = curl_slist_append(curl_jmap_headers, "User-agent: fetchmail-jmap/0.1");
	curl_jmap_headers = curl_slist_append(curl_jmap_headers, "Content-type: application/json");
	curl_easy_setopt(curl_jmap, CURLOPT_HTTPHEADER, curl_jmap_headers);

	curl_easy_setopt(curl_jmap, CURLOPT_URL, "https://proxy.jmap.io/jmap/9e2e4652-3191-11e5-847c-0026b9fac7aa/"); // XXX config
    }

    buf_reset(&body_in);
    buf_reset(&body_out);

    WJWriter writer = WJWOpenDocument(0, _jmap_wjw_callback, 0);
    WJEWriteDocument(batch, writer, 0);
    WJWCloseDocument(writer);

    curl_easy_setopt(curl_jmap, CURLOPT_POSTFIELDS, body_out.s);
    curl_easy_setopt(curl_jmap, CURLOPT_POSTFIELDSIZE, body_out.len);

    CURLcode rc = curl_easy_perform(curl_jmap);
    if (rc != CURLE_OK) {
	report_build(stderr, "fetchmail: jmap: curl request failed: %s\n", curl_easy_strerror(rc));
	return PS_UNDEFINED; // XXX map to internal
    }

    uint32_t code;
    curl_easy_getinfo(curl_jmap, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200) {
	report_build(stderr, "fetchmail: jmap: HTTP request returned: %d\n", code);
	return PS_UNDEFINED; // XXX map to internal
    }

    buf_cstring(&body_in);
    *rbatch = WJEParse(body_in.s);

    return PS_SUCCESS;
}

WJElement message_list = 0;

static int jmap_getrange(int sock, 
			 struct query *ctl, 
			 const char *folder, 
			 int *countp, int *newp, int *bytes) {
    int r = PS_SUCCESS;
    *countp = *newp = *bytes = -1;
    WJElement rbatch = 0, inbox = 0;

    report(stderr, "in jmap_getrange()\n");

    WJElement batch = WJEArray(NULL, NULL, WJE_NEW);
    WJEString(batch, "[0][$]", WJE_SET, "getMailboxes");
    WJEObject(batch, "[0][$]", WJE_SET);
    WJEString(batch, "[0][$]", WJE_SET, "0");

    r = _jmap_call(batch, &rbatch);
    if (r) goto done;

    // XXX folder name search
    inbox = WJEGet(rbatch, "[0][1].list[]; role == 'inbox'", 0);

    *countp = WJEInt32(inbox, "totalMessages", WJE_GET, -1);
    *newp   = WJEInt32(inbox, "unreadMessages", WJE_GET, -1);

    report_build(stderr, "fetchmail: jmap: total %d unread %d\n", *countp, *newp);

    char *folder_id = WJEString(inbox, "id", WJE_GET, 0);

    WJECloseDocument(batch);  batch = 0;
    batch = WJEArray(NULL, NULL, WJE_NEW);
    WJEString(batch, "[0][$]", WJE_SET, "getMessageList");

    WJElement args = WJEObject(batch, "[0][$]", WJE_SET);
    WJEString(args, "sort[$]", WJE_SET, "id");
    WJENumber(args, "position", WJE_SET, 0);
    WJEBool(args, "fetchMessages", WJE_SET, 1);
    WJEString(args, "fetchMessageProperties[$]", WJE_SET, "blobId");

    WJElement filter = WJEObject(args, "filter", WJE_SET);
    WJEString(filter, "inMailboxes[$]", WJE_SET, folder_id);
    WJEBool(filter, "isUnread", WJE_SET, 1);

    WJEString(batch, "[0][$]", WJE_SET, "0");
    WJEDump(batch);

    WJECloseDocument(rbatch); rbatch = 0;
    r = _jmap_call(batch, &rbatch);
    if (r) goto done;

    WJEDump(rbatch);

    message_list = rbatch;
    rbatch = 0; // prevent free

done:
    if (batch) WJECloseDocument(batch);   
    if (rbatch) WJECloseDocument(rbatch);
    return PS_SUCCESS;
}

static int _blob_get(char *blob_id)
{
    if (!curl_blob) {
	curl_blob = curl_easy_init();
	//curl_easy_setopt(curl_blob, CURLOPT_VERBOSE, 1);

	curl_easy_setopt(curl_blob, CURLOPT_READFUNCTION, _jmap_curl_read);
	curl_easy_setopt(curl_blob, CURLOPT_WRITEFUNCTION, _jmap_curl_write);

	curl_blob_headers = curl_slist_append(curl_blob_headers, "User-agent: fetchmail-jmap/0.1");
	curl_easy_setopt(curl_blob, CURLOPT_HTTPHEADER, curl_blob_headers);
    }

    // XXX construct URL from downloadUrl(blob_id)
    static char blob_url[256];
    snprintf(blob_url, sizeof(blob_url), "https://proxy.jmap.io/raw/9e2e4652-3191-11e5-847c-0026b9fac7aa/%s/xxx", blob_id);
    curl_easy_setopt(curl_blob, CURLOPT_URL, blob_url);

    buf_reset(&body_in);
    buf_reset(&body_out);

    CURLcode rc = curl_easy_perform(curl_blob);
    if (rc != CURLE_OK) {
	report_build(stderr, "fetchmail: jmap: curl request failed: %s\n", curl_easy_strerror(rc));
	return PS_UNDEFINED; // XXX map to internal
    }

    uint32_t code;
    curl_easy_getinfo(curl_blob, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200) {
	report_build(stderr, "fetchmail: jmap: HTTP request returned: %d\n", code);
	return PS_UNDEFINED; // XXX map to internal
    }

    buf_cstring(&body_in);

    return PS_SUCCESS;
}

static int jmap_fetch(int sock, struct query *ctl, int number, int *lenp)
{
    int r = PS_SUCCESS;
    static char selector[128];

    report(stderr, "in jmap_fetch()\n");

    snprintf(selector, sizeof(selector), "[0][1].messageIds[%d]", number-1); // sigh, IMAP counts from 1
    char *msg_id = WJEString(message_list, selector, WJE_GET, "");
    snprintf(selector, sizeof(selector), "[1][1].list[]; id == '%s'", msg_id);
    WJElement message = WJEObject(message_list, selector, WJE_GET);
    char *blob_id = WJEString(message, "blobId", WJE_GET, "");

    report_build(stderr, "fetchmail: jmap: mapped item %d to %s (%s)\n", number, msg_id, blob_id);

    r = _blob_get(blob_id);
    if (r) goto done;

    struct buf *bufsock = BufSock();
    buf_copy(bufsock, &body_in);
    buf_appendmap(bufsock, ".\r\n", 3);
    *lenp = buf_len(bufsock);

done:
    return r;
}

static int jmap_logout(int sock, struct query *ctl)
{
    report(stderr, "in jmap_logout()\n");
    return PS_SUCCESS;
}

static const struct method jmap =
{
    "JMAP",				/* Post Office Protocol v2 */
    "http",				/* standard POP2 port */
    "https",				/* ssl POP2 port - not */
    FALSE,				/* this is not a tagged protocol */
    TRUE,				/* does not use message delimiter */
    jmap_ok,				/* parse command response */
    NULL,			/* get authorization */
    jmap_getrange,			/* query range of messages */
    NULL,				/* no way to get sizes */
    NULL,				/* no way to get sizes of subsets */
    NULL,				/* messages are always new */
    jmap_fetch,				/* request given message */
    NULL,				/* no way to fetch body alone */
    NULL,				/* eat message trailer */
    NULL,				/* no POP2 delete method */
    NULL,				/* how to mark a message as seen */
    NULL,				/* how to end mailbox processing */
    jmap_logout,			/* log out, we're done */
    FALSE				/* no, we can't re-poll */
};

int doJMAP (struct query *ctl)
/* retrieve messages using JMAP */
{
    curl_global_init(CURL_GLOBAL_ALL);
    return(do_protocol(ctl, &jmap));
}
#endif /* JMAP_ENABLE */

/* jmap.c ends here */
