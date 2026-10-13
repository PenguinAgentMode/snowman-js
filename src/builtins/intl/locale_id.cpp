// Language tags: IsStructurallyValidLanguageTag, CanonicalizeUnicodeLocaleId (ECMA-402 6.2) over UTS 35
// unicode_locale_id, alias replacement (UTS 35 Annex C) and likely subtags. Data: locale_tables.inc.
#include <algorithm>
#include <cstring>

#include "builtins/intl/intl.h"

namespace snowman::intl {

namespace {

struct AliasEntry {
  const char* from;
  const char* to;
};
#include "builtins/intl/locale_tables.inc"

template <size_t N>
const char* lookup(const AliasEntry (&table)[N], std::string_view key) {
  const AliasEntry* end = table + N;
  const AliasEntry* it =
      std::lower_bound(table, end, key, [](const AliasEntry& e, std::string_view k) { return std::string_view(e.from) < k; });
  return it != end && std::string_view(it->from) == key ? it->to : nullptr;
}

bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlnum(char c) { return isAlpha(c) || isDigit(c); }
bool allAlpha(std::string_view s) { return std::all_of(s.begin(), s.end(), isAlpha); }
bool allAlnum(std::string_view s) { return std::all_of(s.begin(), s.end(), isAlnum); }
bool allDigit(std::string_view s) { return std::all_of(s.begin(), s.end(), isDigit); }
bool alnumOfLength(std::string_view s, size_t min, size_t max) { return s.size() >= min && s.size() <= max && allAlnum(s); }

std::string titlecase(std::string_view s) {
  std::string out = asciiLower(s);
  if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') out[0] = char(out[0] - 32);
  return out;
}

// Parses language (script)? (region)? (variant)* starting at parts[i]; false on a malformed language subtag or a
// duplicate variant. Stops at the first subtag that fits none of them.
bool parseLanguageId(const std::vector<std::string>& parts, size_t& i, LanguageId& id) {
  if (i >= parts.size() || !isUnicodeLanguageSubtag(parts[i])) return false;
  id.language = parts[i++];
  if (i < parts.size() && isUnicodeScriptSubtag(parts[i])) id.script = titlecase(parts[i++]);
  if (i < parts.size() && isUnicodeRegionSubtag(parts[i])) id.region = asciiUpper(parts[i++]);
  while (i < parts.size() && isUnicodeVariantSubtag(parts[i])) {
    if (std::find(id.variants.begin(), id.variants.end(), parts[i]) != id.variants.end()) return false;
    id.variants.push_back(parts[i++]);
  }
  return true;
}

// The ordering of extensions: digits, then letters (the singleton's byte order), private use last.
void appendExtension(std::string& out, char singleton, const std::string& body) {
  out += '-';
  out += singleton;
  if (!body.empty()) {
    out += '-';
    out += body;
  }
}

void sortVariants(LanguageId& id) { std::sort(id.variants.begin(), id.variants.end()); }

// ---- alias replacement ----

struct Rule {
  LanguageId from;
  LanguageId to;
  int priority;
};

LanguageId parseIdOrDie(std::string_view s) {
  std::vector<std::string> parts = split(asciiLower(s), '-');
  size_t i = 0;
  LanguageId id;
  parseLanguageId(parts, i, id);
  return id;
}

// The complex languageAlias rules, parsed once. Priority: a specific language, then a region, then variants
// (ICU's AliasReplacer order).
const std::vector<Rule>& complexRules() {
  static const std::vector<Rule> rules = [] {
    std::vector<Rule> out;
    for (const AliasEntry& e : kComplexLanguageAliases) {
      Rule r{parseIdOrDie(e.from), parseIdOrDie(e.to), 0};
      r.priority = (r.from.language != "und" ? 4 : 0) + (r.from.region.empty() ? 0 : 2) + (r.from.variants.empty() ? 0 : 1);
      out.push_back(std::move(r));
    }
    return out;
  }();
  return rules;
}

bool hasVariant(const LanguageId& id, const std::string& v) {
  return std::find(id.variants.begin(), id.variants.end(), v) != id.variants.end();
}

bool ruleMatches(const LanguageId& from, const LanguageId& id) {
  if (from.language != "und" && from.language != id.language) return false;
  if (!from.script.empty() && from.script != id.script) return false;
  if (!from.region.empty() && from.region != id.region) return false;
  for (const std::string& v : from.variants)
    if (!hasVariant(id, v)) return false;
  return true;
}

void applyRule(const LanguageId& from, const LanguageId& to, LanguageId& id) {
  if (from.language != "und" || to.language != "und") id.language = to.language;
  if (!from.script.empty() || id.script.empty()) id.script = to.script;
  if (!from.region.empty() || id.region.empty()) id.region = to.region;
  std::vector<std::string> variants;
  for (const std::string& v : id.variants)
    if (!hasVariant(from, v)) variants.push_back(v);
  for (const std::string& v : to.variants)
    if (std::find(variants.begin(), variants.end(), v) == variants.end()) variants.push_back(v);
  id.variants = std::move(variants);
  sortVariants(id);
}

// One alias replacement step; false when no rule applies.
bool replaceOnce(LanguageId& id) {
  // languageAlias: the best complex rule, or the simple rule for the language subtag.
  const Rule* best = nullptr;
  for (const Rule& r : complexRules()) {
    if (!ruleMatches(r.from, id)) continue;
    if (!best || r.priority > best->priority) best = &r;
  }
  const char* simple = lookup(kLanguageAliases, id.language);
  if (simple && (!best || best->priority <= 4)) {
    LanguageId from;
    from.language = id.language;
    applyRule(from, parseIdOrDie(simple), id);
    return true;
  }
  if (best) {
    applyRule(best->from, best->to, id);
    return true;
  }
  if (!id.script.empty()) {
    if (const char* to = lookup(kScriptAliases, id.script)) {
      id.script = to;
      return true;
    }
  }
  if (!id.region.empty()) {
    if (const char* to = lookup(kRegionAliases, id.region)) {
      std::vector<std::string> candidates = split(to, ' ');
      std::string replacement = candidates[0];
      if (candidates.size() > 1) {
        LanguageId probe;
        probe.language = id.language;
        probe.script = id.script;
        if (AddLikelySubtags(probe) &&
            std::find(candidates.begin(), candidates.end(), probe.region) != candidates.end())
          replacement = probe.region;
      }
      id.region = replacement;
      return true;
    }
  }
  for (std::string& v : id.variants) {
    if (const char* to = lookup(kVariantAliases, v)) {
      v = to;
      sortVariants(id);
      id.variants.erase(std::unique(id.variants.begin(), id.variants.end()), id.variants.end());
      return true;
    }
  }
  return false;
}

// A subdivision alias may be a region code: as a subdivision value it becomes "<region>zzzz".
std::string canonicalizeSubdivision(std::string_view value) {
  const char* to = lookup(kSubdivisionAliases, value);
  if (!to) return std::string(value);
  std::string first = split(to, ' ')[0];
  if (isUnicodeRegionSubtag(first)) return asciiLower(first) + "zzzz";
  return first;
}

// ---- likely subtags ----

const std::vector<std::pair<std::string_view, std::string_view>>& likelyIndex() {
  static const std::vector<std::pair<std::string_view, std::string_view>> index = [] {
    std::vector<std::pair<std::string_view, std::string_view>> out;
    std::string_view blob(kLikelySubtags, sizeof(kLikelySubtags) - 1);
    while (!blob.empty()) {
      size_t nl = blob.find('\n');
      std::string_view line = blob.substr(0, nl);
      size_t sp = line.find(' ');
      out.emplace_back(line.substr(0, sp), line.substr(sp + 1));
      blob.remove_prefix(nl + 1);
    }
    return out;
  }();
  return index;
}

std::optional<std::string_view> likely(std::string_view key) {
  const auto& index = likelyIndex();
  auto it = std::lower_bound(index.begin(), index.end(), key,
                             [](const std::pair<std::string_view, std::string_view>& e, std::string_view k) { return e.first < k; });
  if (it != index.end() && it->first == key) return it->second;
  return std::nullopt;
}

}  // namespace

// ---- helpers ----

std::string asciiLower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return out;
}

std::string asciiUpper(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'a' && c <= 'z') c = char(c - 32);
  return out;
}

std::optional<std::string> toAscii(std::u16string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char16_t c : s) {
    if (c >= 0x80) return std::nullopt;
    out.push_back(char(c));
  }
  return out;
}

String* jsStr(std::string_view ascii) { return newString(builtins::toU16(ascii)); }

std::vector<std::string> split(std::string_view s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    out.emplace_back(s.substr(start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

bool isUnicodeLanguageSubtag(std::string_view s) {
  return ((s.size() >= 2 && s.size() <= 3) || (s.size() >= 5 && s.size() <= 8)) && allAlpha(s);
}
bool isUnicodeScriptSubtag(std::string_view s) { return s.size() == 4 && allAlpha(s); }
bool isUnicodeRegionSubtag(std::string_view s) {
  return (s.size() == 2 && allAlpha(s)) || (s.size() == 3 && allDigit(s));
}
bool isUnicodeVariantSubtag(std::string_view s) {
  return alnumOfLength(s, 5, 8) || (s.size() == 4 && isDigit(s[0]) && allAlnum(s));
}
bool isUnicodeLocaleType(std::string_view s) {
  for (const std::string& part : split(s, '-'))
    if (!alnumOfLength(part, 3, 8)) return false;
  return true;
}

// ---- LanguageId / LanguageTag ----

std::string LanguageId::toString() const {
  std::string out = language;
  if (!script.empty()) out += "-" + script;
  if (!region.empty()) out += "-" + region;
  for (const std::string& v : variants) out += "-" + v;
  return out;
}

const std::string* LanguageTag::keyword(std::string_view key) const {
  for (const Keyword& k : keywords)
    if (k.key == key) return &k.value;
  return nullptr;
}

std::string LanguageTag::toString() const {
  std::string out = id.toString();
  std::vector<std::pair<char, std::string>> exts = otherExtensions;
  if (hasTransformedExtension) {
    std::string body;
    if (tlang) body = asciiLower(tlang->toString());
    for (const Keyword& f : tfields) {
      if (!body.empty()) body += '-';
      body += f.key + "-" + f.value;
    }
    exts.emplace_back('t', body);
  }
  if (hasUnicodeExtension) {
    std::string body;
    for (const std::string& a : attributes) {
      if (!body.empty()) body += '-';
      body += a;
    }
    for (const Keyword& k : keywords) {
      if (!body.empty()) body += '-';
      body += k.key;
      if (!k.value.empty()) body += "-" + k.value;
    }
    exts.emplace_back('u', body);
  }
  std::stable_sort(exts.begin(), exts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [singleton, body] : exts) appendExtension(out, singleton, body);
  if (!privateUse.empty()) out += "-x-" + privateUse;
  return out;
}

std::optional<LanguageTag> ParseLanguageTag(std::u16string_view tag) {
  std::optional<std::string> ascii = toAscii(tag);
  if (!ascii) return std::nullopt;
  return ParseLanguageTag(std::string_view(*ascii));
}

// 6.2.1 IsStructurallyValidLanguageTag ( locale ), parsing as it goes.
std::optional<LanguageTag> ParseLanguageTag(std::string_view input) {
  std::vector<std::string> parts = split(asciiLower(input), '-');
  for (const std::string& p : parts)
    if (p.empty() || !allAlnum(p)) return std::nullopt;
  LanguageTag tag;
  size_t i = 0;
  if (!parseLanguageId(parts, i, tag.id)) return std::nullopt;
  std::string seen;
  while (i < parts.size()) {
    if (parts[i].size() != 1) return std::nullopt;
    char singleton = parts[i][0];
    i++;
    if (singleton == 'x') {
      if (i == parts.size()) return std::nullopt;
      std::vector<std::string> rest;
      for (; i < parts.size(); i++) {
        if (!alnumOfLength(parts[i], 1, 8)) return std::nullopt;
        rest.push_back(parts[i]);
      }
      tag.privateUse = join(rest, "-");
      break;
    }
    if (seen.find(singleton) != std::string::npos) return std::nullopt;
    seen += singleton;
    if (singleton == 'u') {
      tag.hasUnicodeExtension = true;
      size_t start = i;
      while (i < parts.size() && alnumOfLength(parts[i], 3, 8)) tag.attributes.push_back(parts[i++]);
      while (i < parts.size() && parts[i].size() == 2) {
        if (!isAlpha(parts[i][1])) return std::nullopt;
        Keyword k{parts[i++], {}};
        std::vector<std::string> type;
        while (i < parts.size() && alnumOfLength(parts[i], 3, 8)) type.push_back(parts[i++]);
        k.value = join(type, "-");
        tag.keywords.push_back(std::move(k));
      }
      if (i == start) return std::nullopt;
    } else if (singleton == 't') {
      tag.hasTransformedExtension = true;
      size_t start = i;
      if (i < parts.size() && isUnicodeLanguageSubtag(parts[i])) {
        LanguageId tlang;
        if (!parseLanguageId(parts, i, tlang)) return std::nullopt;
        tag.tlang = std::move(tlang);
      }
      while (i < parts.size() && parts[i].size() == 2) {
        if (!isAlpha(parts[i][0]) || !isDigit(parts[i][1])) return std::nullopt;
        Keyword f{parts[i++], {}};
        std::vector<std::string> value;
        while (i < parts.size() && alnumOfLength(parts[i], 3, 8)) value.push_back(parts[i++]);
        if (value.empty()) return std::nullopt;
        f.value = join(value, "-");
        tag.tfields.push_back(std::move(f));
      }
      if (i == start) return std::nullopt;
    } else {
      std::vector<std::string> body;
      while (i < parts.size() && alnumOfLength(parts[i], 2, 8)) body.push_back(parts[i++]);
      if (body.empty()) return std::nullopt;
      tag.otherExtensions.emplace_back(singleton, join(body, "-"));
    }
  }
  return tag;
}

void ReplaceLanguageIdAliases(LanguageId& id) {
  sortVariants(id);
  for (int guard = 0; guard < 64 && replaceOnce(id); guard++) {
  }
}

std::string CanonicalizeUValue(std::string_view key, std::string_view value) {
  std::string k(key);
  if (k == "rg" || k == "sd") return canonicalizeSubdivision(value);
  std::string probe = k + "-" + std::string(value);
  if (const char* to = lookup(kUnicodeValueAliases, probe)) return to;
  return std::string(value);
}

// 6.2.3 CanonicalizeUnicodeLocaleId ( locale )
void CanonicalizeUnicodeLocaleId(LanguageTag& tag) {
  ReplaceLanguageIdAliases(tag.id);
  if (tag.hasUnicodeExtension) {
    std::vector<std::string> attributes;
    for (const std::string& a : tag.attributes)
      if (std::find(attributes.begin(), attributes.end(), a) == attributes.end()) attributes.push_back(a);
    std::sort(attributes.begin(), attributes.end());
    tag.attributes = std::move(attributes);
    std::vector<Keyword> keywords;
    for (Keyword& k : tag.keywords) {
      bool duplicate = false;
      for (const Keyword& seen : keywords) duplicate |= seen.key == k.key;
      if (duplicate) continue;
      if (!k.value.empty()) k.value = CanonicalizeUValue(k.key, k.value);
      if (k.value == "true") k.value.clear();
      keywords.push_back(std::move(k));
    }
    std::stable_sort(keywords.begin(), keywords.end(), [](const Keyword& a, const Keyword& b) { return a.key < b.key; });
    tag.keywords = std::move(keywords);
  }
  if (tag.hasTransformedExtension) {
    if (tag.tlang) ReplaceLanguageIdAliases(*tag.tlang);
    for (Keyword& f : tag.tfields) {
      if (const char* to = lookup(kTransformValueAliases, f.key + "-" + f.value)) f.value = to;
    }
    std::stable_sort(tag.tfields.begin(), tag.tfields.end(), [](const Keyword& a, const Keyword& b) { return a.key < b.key; });
  }
  std::stable_sort(tag.otherExtensions.begin(), tag.otherExtensions.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
}

std::optional<std::string> CanonicalizeLanguageTag(std::string_view input) {
  std::optional<LanguageTag> tag = ParseLanguageTag(input);
  if (!tag) return std::nullopt;
  CanonicalizeUnicodeLocaleId(*tag);
  return tag->toString();
}

std::string removeUnicodeExtensions(std::string_view tag, std::string* extension) {
  std::vector<std::string> parts = split(tag, '-');
  std::vector<std::string> kept, ext;
  bool inU = false, inPrivate = false;
  for (const std::string& p : parts) {
    if (!inPrivate && p.size() == 1) {
      inU = p == "u";
      inPrivate = p == "x";
    }
    (inU ? ext : kept).push_back(p);
  }
  if (extension) *extension = ext.empty() ? std::string() : "-" + join(ext, "-");
  return join(kept, "-");
}

// UTS 35 Add Likely Subtags (after canonicalization).
bool AddLikelySubtags(LanguageId& id) {
  LanguageId in = id;
  if (in.script == "Zzzz") in.script.clear();
  if (in.region == "ZZ") in.region.clear();
  std::optional<std::string_view> match;
  const std::string& l = in.language;
  if (!in.script.empty() && !in.region.empty()) match = likely(l + "-" + in.script + "-" + in.region);
  if (!match && !in.region.empty()) match = likely(l + "-" + in.region);
  if (!match && !in.script.empty()) match = likely(l + "-" + in.script);
  if (!match) match = likely(l);
  if (!match && l != "und") {
    if (!in.script.empty() && !in.region.empty()) match = likely("und-" + in.script + "-" + in.region);
    if (!match && !in.region.empty()) match = likely("und-" + in.region);
    if (!match && !in.script.empty()) match = likely("und-" + in.script);
  }
  if (!match) return false;
  LanguageId m = parseIdOrDie(*match);
  if (in.language == "und") in.language = m.language;
  if (in.script.empty()) in.script = m.script;
  if (in.region.empty()) in.region = m.region;
  id.language = in.language;
  id.script = in.script;
  id.region = in.region;
  return true;
}

// UTS 35 Remove Likely Subtags, favoring region.
bool RemoveLikelySubtags(LanguageId& id) {
  LanguageId max = id;
  if (!AddLikelySubtags(max)) return false;
  max.variants.clear();
  LanguageId trials[3];
  trials[0].language = max.language;
  trials[1].language = max.language;
  trials[1].region = max.region;
  trials[2].language = max.language;
  trials[2].script = max.script;
  for (LanguageId& trial : trials) {
    LanguageId probe = trial;
    if (AddLikelySubtags(probe) && probe.language == max.language && probe.script == max.script &&
        probe.region == max.region) {
      id.language = trial.language;
      id.script = trial.script;
      id.region = trial.region;
      return true;
    }
  }
  id.language = max.language;
  id.script = max.script;
  id.region = max.region;
  return true;
}

}  // namespace snowman::intl
