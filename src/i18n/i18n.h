// Minimal i18n: every user-facing string passes through _L().  If the
// current language has a translation, it is returned; otherwise the
// English fallback (the argument itself) is used.  No external files
// needed — all translations are embedded.
#pragma once

enum class Lang : int
{
    EN = 0,  // English (default)
    DE,      // German
    FR,      // French
    ES,      // Spanish
    RU,      // Russian
    PT,      // Portuguese
    IT,      // Italian
    NL,      // Dutch
    JA,      // Japanese
    ZH,      // Chinese (Simplified)
    KO,      // Korean
    PL,      // Polish
    SV,      // Swedish
    TR,      // Turkish
    AR,      // Arabic
    KOUNT,
};

// Call once at startup.
void i18nInit();

// Switch active language (rebuilds the lookup map).
void i18nSet(Lang lang);
Lang i18nGet();

// Human-readable name for a language enum value.
const char* i18nName(Lang lang);

// Look up the translation for the given English string.  Returns the
// translation if available, or 'en' unchanged.  Safe to call from any
// thread once i18nInit() has run.
const char* _L(const char* en);
