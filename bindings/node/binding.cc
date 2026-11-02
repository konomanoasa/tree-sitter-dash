#include <napi.h>

struct TSLanguage;
extern "C" const TSLanguage *tree_sitter_dash();

namespace {

constexpr napi_type_tag language_type_tag = {
  0x8AF2E5212AD58ABF,
  0xD5006CAD83ABBA16
};

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  auto language = Napi::External<TSLanguage>::New(
    env,
    const_cast<TSLanguage *>(tree_sitter_dash())
  );
  language.TypeTag(&language_type_tag);
  exports["language"] = language;
  return exports;
}

}

NODE_API_MODULE(tree_sitter_dash_binding, Init)
