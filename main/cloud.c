/* Cloud classifier dispatcher — see cloud.h. Thin routing over claude.c and
 * gemini.c; the "one active at a time" rule lives here so no caller has to know
 * which provider is selected. */
#include "cloud.h"
#include "claude.h"
#include "gemini.h"
#include "settings.h"

cloud_provider_t cloud_active(void)
{
    cloud_provider_t p = (cloud_provider_t) g_settings.cloud_provider;
    if (p != CLOUD_OFF && !cloud_have_key(p)) return CLOUD_OFF;
    return p;
}

bool cloud_enabled(void)
{
    return cloud_active() != CLOUD_OFF;
}

bool cloud_have_key(cloud_provider_t p)
{
    switch (p) {
    case CLOUD_CLAUDE: return claude_have_key();
    case CLOUD_GEMINI: return gemini_have_key();
    default:           return false;
    }
}

const char *cloud_name(cloud_provider_t p)
{
    switch (p) {
    case CLOUD_CLAUDE: return "Claude";
    case CLOUD_GEMINI: return "Gemini";
    default:           return "none";
    }
}

const char *cloud_source_tag(cloud_provider_t p)
{
    switch (p) {
    case CLOUD_CLAUDE: return "claude";
    case CLOUD_GEMINI: return "gemini";
    default:           return "";
    }
}

cloud_provider_t cloud_for_manual(void)
{
    cloud_provider_t p = cloud_active();
    if (p != CLOUD_OFF) return p;
    if (claude_have_key()) return CLOUD_CLAUDE;
    if (gemini_have_key()) return CLOUD_GEMINI;
    return CLOUD_OFF;
}

esp_err_t cloud_classify_file(cloud_provider_t p, const char *fs_path,
                              classify_result_t *out)
{
    switch (p) {
    case CLOUD_CLAUDE: return claude_classify_file(fs_path, out);
    case CLOUD_GEMINI: return gemini_classify_file(fs_path, out);
    default:           return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t cloud_test(cloud_provider_t p, char *out, size_t osz)
{
    switch (p) {
    case CLOUD_CLAUDE: return claude_test(out, osz);
    case CLOUD_GEMINI: return gemini_test(out, osz);
    default:           return ESP_ERR_INVALID_STATE;
    }
}

const char *cloud_last_error(cloud_provider_t p)
{
    switch (p) {
    case CLOUD_CLAUDE: return claude_last_error();
    case CLOUD_GEMINI: return gemini_last_error();
    default:           return "";
    }
}
