{
  "targets": [
    {
      "target_name": "tree_sitter_dash_binding",
      "sources": [
        "bindings/node/binding.cc",
        "src/parser.c",
        "src/scanner.c"
      ],
      "include_dirs": ["src"],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').targets\"):node_addon_api_except"
      ],
      "cflags_c": ["-std=c17"],
      "cflags_cc": ["-std=c++17"],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "AdditionalOptions": ["/utf-8"],
          "AdditionalOptions!": ["-std:c++20"],
          "LanguageStandard": "stdcpp17",
          "LanguageStandard_C": "stdc17"
        }
      },
      "target_conditions": [
        [
          "OS==\"mac\"",
          {
            "xcode_settings": {
              "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
              "GCC_C_LANGUAGE_STANDARD": "c17",
              "MACOSX_DEPLOYMENT_TARGET": "14.0"
            }
          }
        ]
      ]
    }
  ]
}
