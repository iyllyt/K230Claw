/*
 * tool_web.c - Web Search + Web Fetch 工具
 *
 * web_search: Bing 搜索（国内可直连，无需 API key）
 *   - 请求 https://cn.bing.com/search?q=... 获取搜索结果
 *   - 解析 HTML 提取结果标题、URL 和摘要
 *
 * web_fetch: libcurl GET 获取网页内容
 *   - 简单 HTML 标签剥离，返回纯文本
 *   - 输出截断到 max_length（默认 4096）
 */

#include "tool_web.h"
#include "../kc_config.h"
#include "../third_party/cJSON/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <curl/curl.h>

#define TAG "tool_web"
#define DEFAULT_MAX_LENGTH  4096
#define FETCH_MAX_SIZE      (64 * 1024)
#define SEARCH_MAX_RESULTS  5

/* ── libcurl 响应缓冲区 ── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} web_buf_t;

static size_t web_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    web_buf_t *buf = (web_buf_t *)userdata;
    size_t bytes = size * nmemb;
    if (buf->len + bytes >= buf->cap) {
        if (buf->cap >= FETCH_MAX_SIZE) return 0; /* 停止接收 */
        size_t new_cap = buf->cap * 2;
        if (new_cap > FETCH_MAX_SIZE) new_cap = FETCH_MAX_SIZE;
        char *tmp = realloc(buf->data, new_cap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';
    return bytes;
}

static web_buf_t *web_fetch_url(const char *url, long timeout_sec) {
    web_buf_t *buf = calloc(1, sizeof(web_buf_t));
    if (!buf) return NULL;
    buf->cap = 8192;
    buf->data = malloc(buf->cap);
    if (!buf->data) { free(buf); return NULL; }
    buf->data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) { free(buf->data); free(buf); return NULL; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                     "Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    /* 代理 */
    const char *proxy = kc_config_get_str("proxy", "");
    if (proxy[0])
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        KC_LOGW(TAG, "fetch '%s' failed: %s", url, curl_easy_strerror(res));
        free(buf->data);
        free(buf);
        return NULL;
    }

    return buf;
}

static void web_buf_free(web_buf_t *buf) {
    if (buf) { free(buf->data); free(buf); }
}

/* ── HTML 标签剥离（简单版） ── */

static char *strip_html_tags(const char *html, size_t max_len) {
    size_t html_len = strlen(html);
    char *out = malloc(html_len + 1);
    if (!out) return NULL;

    size_t oi = 0;
    int in_tag = 0;
    int in_script = 0;
    int in_style = 0;
    int prev_space = 0;

    for (size_t i = 0; i < html_len && oi < max_len; i++) {
        char c = html[i];

        if (c == '<') {
            in_tag = 1;
            /* 检查 script/style */
            if (i + 7 < html_len) {
                if (strncasecmp(html + i, "<script", 7) == 0) in_script = 1;
                if (strncasecmp(html + i, "<style", 6) == 0) in_style = 1;
            }
            if (i + 8 < html_len) {
                if (strncasecmp(html + i, "</script", 8) == 0) in_script = 0;
                if (strncasecmp(html + i, "</style", 7) == 0) in_style = 0;
            }
            continue;
        }
        if (c == '>') { in_tag = 0; continue; }
        if (in_tag || in_script || in_style) continue;

        /* HTML 实体简单处理 */
        if (c == '&') {
            if (strncmp(html + i, "&amp;", 5) == 0) { out[oi++] = '&'; i += 4; }
            else if (strncmp(html + i, "&lt;", 4) == 0) { out[oi++] = '<'; i += 3; }
            else if (strncmp(html + i, "&gt;", 4) == 0) { out[oi++] = '>'; i += 3; }
            else if (strncmp(html + i, "&quot;", 6) == 0) { out[oi++] = '"'; i += 5; }
            else if (strncmp(html + i, "&nbsp;", 6) == 0) { out[oi++] = ' '; i += 5; }
            else if (strncmp(html + i, "&#39;", 5) == 0) { out[oi++] = '\''; i += 4; }
            else out[oi++] = '&';
            prev_space = 0;
            continue;
        }

        /* 压缩空白 */
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && prev_space) continue;
        prev_space = (c == ' ');

        out[oi++] = c;
    }
    out[oi] = '\0';
    return out;
}

/* ── 提取 HTML 标签之间的文本 ── */

static size_t extract_tag_text(const char *start, const char *end,
                               char *out, size_t out_size)
{
    if (!start || !end || end <= start || !out) return 0;
    size_t raw_len = end - start;
    char *raw = malloc(raw_len + 1);
    if (!raw) return 0;
    memcpy(raw, start, raw_len);
    raw[raw_len] = '\0';
    char *stripped = strip_html_tags(raw, out_size - 1);
    free(raw);
    if (!stripped) return 0;
    /* 去除首尾空白 */
    char *s = stripped;
    while (*s == ' ') s++;
    size_t slen = strlen(s);
    while (slen > 0 && s[slen - 1] == ' ') slen--;
    if (slen >= out_size) slen = out_size - 1;
    memcpy(out, s, slen);
    out[slen] = '\0';
    free(stripped);
    return slen;
}

/* ── Bing 搜索结果解析 ── */

/*
 * Bing HTML 搜索结果结构（cn.bing.com/search?q=...）:
 *
 * 每条结果在 <li class="b_algo"> 内:
 *   <h2><a href="实际URL">标题</a></h2>
 *   <p class="b_lineclamp...">摘要文本...</p>
 *   或 <div class="b_caption"><p>摘要文本</p></div>
 */
static char *parse_bing_results(const char *html, int max_results) {
    size_t buf_size = 4096;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;
    size_t off = 0;

    const char *p = html;
    int count = 0;

    while (count < max_results) {
        /* 查找下一个搜索结果块 */
        p = strstr(p, "class=\"b_algo\"");
        if (!p) break;

        /* 限定搜索范围到下一个 b_algo 或合理距离 */
        const char *block_end = strstr(p + 14, "class=\"b_algo\"");
        if (!block_end) block_end = p + 4096;

        /* 提取 URL：找 <h2><a href="..." */
        char result_url[512] = "";
        const char *h2 = strstr(p, "<h2");
        if (h2 && h2 < block_end) {
            const char *a_href = strstr(h2, "href=\"");
            if (a_href && a_href < block_end) {
                a_href += 6;
                const char *href_end = strchr(a_href, '"');
                if (href_end) {
                    size_t ulen = href_end - a_href;
                    if (ulen >= sizeof(result_url)) ulen = sizeof(result_url) - 1;
                    memcpy(result_url, a_href, ulen);
                    result_url[ulen] = '\0';
                }
            }
        }

        /* 提取标题：<a href="...">标题</a> */
        char title[256] = "";
        if (h2 && h2 < block_end) {
            const char *a_start = strchr(h2, '>');  /* <h2> 或 <h2 class=...> 的 > */
            if (a_start) {
                /* 找 <a ...> 的闭合 > */
                const char *a_text = strchr(a_start + 1, '>');
                if (a_text && a_text < block_end) {
                    a_text++;
                    const char *a_close = strstr(a_text, "</a>");
                    if (a_close && a_close < block_end) {
                        extract_tag_text(a_text, a_close, title, sizeof(title));
                    }
                }
            }
        }

        /* 提取摘要：多种可能的容器 */
        char snippet[512] = "";
        /* 方式1：<p class="b_lineclamp..."> */
        const char *snip_p = strstr(p, "class=\"b_lineclamp");
        if (!snip_p || snip_p >= block_end) {
            /* 方式2：<div class="b_caption"><p> */
            snip_p = strstr(p, "class=\"b_caption\"");
        }
        if (snip_p && snip_p < block_end) {
            const char *snip_start = strchr(snip_p, '>');
            if (snip_start) {
                snip_start++;
                /* 如果是 b_caption，再找内层 <p> */
                if (strncmp(snip_p + 7, "b_caption", 9) == 0) {
                    const char *inner_p = strstr(snip_start, "<p");
                    if (inner_p && inner_p < block_end) {
                        snip_start = strchr(inner_p, '>');
                        if (snip_start) snip_start++;
                    }
                }
                if (snip_start) {
                    const char *snip_end = strstr(snip_start, "</p>");
                    if (!snip_end || snip_end >= block_end)
                        snip_end = strstr(snip_start, "</div>");
                    if (snip_end && snip_end < block_end) {
                        extract_tag_text(snip_start, snip_end,
                                         snippet, sizeof(snippet));
                    }
                }
            }
        }

        if (title[0]) {
            size_t needed = strlen(title) + strlen(result_url) +
                            strlen(snippet) + 32;
            if (off + needed >= buf_size) {
                buf_size *= 2;
                char *tmp = realloc(buf, buf_size);
                if (!tmp) break;
                buf = tmp;
            }
            off += snprintf(buf + off, buf_size - off,
                            "%d. %s\n   URL: %s\n   %s\n\n",
                            count + 1, title, result_url, snippet);
            count++;
        }

        p = (block_end < p + 4096) ? block_end : p + 1;
    }

    if (count == 0) {
        snprintf(buf, buf_size, "No results found.");
    }

    return buf;
}

/* ── 公共 API ── */

kc_tool_result_t *tool_web_search_execute(const char *input_json)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) return tool_result_error("Invalid JSON input");

    const char *query = cJSON_GetStringValue(cJSON_GetObjectItem(root, "query"));
    if (!query || !query[0]) {
        cJSON_Delete(root);
        return tool_result_error("Missing 'query' parameter");
    }

    cJSON *max_item = cJSON_GetObjectItem(root, "max_results");
    int max_results = (max_item && cJSON_IsNumber(max_item)) ?
                      (int)max_item->valuedouble : SEARCH_MAX_RESULTS;
    if (max_results < 1) max_results = 1;
    if (max_results > 10) max_results = 10;

    /* URL 编码查询 */
    CURL *curl_handle = curl_easy_init();
    char *encoded = curl_easy_escape(curl_handle, query, 0);
    curl_easy_cleanup(curl_handle);

    if (!encoded) {
        cJSON_Delete(root);
        return tool_result_error("Failed to encode query");
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://cn.bing.com/search?q=%s&ensearch=0", encoded);
    curl_free(encoded);

    KC_LOGI(TAG, "web_search: '%s'", query);
    cJSON_Delete(root);

    web_buf_t *resp = web_fetch_url(url, 15);
    if (!resp) return tool_result_error("Search request failed (network error)");

    char *results = parse_bing_results(resp->data, max_results);
    web_buf_free(resp);

    if (!results) return tool_result_error("Failed to parse search results");

    kc_tool_result_t *r = tool_result_text(results);
    free(results);
    return r;
}

kc_tool_result_t *tool_web_fetch_execute(const char *input_json)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) return tool_result_error("Invalid JSON input");

    const char *url_raw = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    if (!url_raw || !url_raw[0]) {
        cJSON_Delete(root);
        return tool_result_error("Missing 'url' parameter");
    }

    /* 拷贝 URL 到本地缓冲区，因为 cJSON_Delete 会释放原始字符串 */
    char url[2048];
    snprintf(url, sizeof(url), "%s", url_raw);

    cJSON *len_item = cJSON_GetObjectItem(root, "max_length");
    int max_length = (len_item && cJSON_IsNumber(len_item)) ?
                     (int)len_item->valuedouble : DEFAULT_MAX_LENGTH;
    if (max_length < 100) max_length = 100;
    if (max_length > FETCH_MAX_SIZE) max_length = FETCH_MAX_SIZE;

    KC_LOGI(TAG, "web_fetch: '%s' (max=%d)", url, max_length);
    cJSON_Delete(root);

    web_buf_t *resp = web_fetch_url(url, 30);
    if (!resp) return tool_result_error("Fetch failed (network error)");

    /* 剥离 HTML 标签 */
    char *text = strip_html_tags(resp->data, max_length);
    web_buf_free(resp);

    if (!text) return tool_result_error("Failed to process page content");

    kc_tool_result_t *r = tool_result_text(text);
    free(text);
    return r;
}
