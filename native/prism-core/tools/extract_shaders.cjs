// Writes Core's runtime shader source (kShaders in src/pipeline.cpp, with the looks of
// src/look_hlsl.h spliced in where pipeline.cpp splices PRISM_LOOK_HLSL) to build/shaders.hlsl,
// for shader_check to compile every entry point at build time.
const fs = require('fs');
const src = fs.readFileSync('src/pipeline.cpp', 'utf8');
const look = fs.readFileSync('src/look_hlsl.h', 'utf8');
const lm = '#define PRISM_LOOK_HLSL R"(';
const lookHlsl = look.slice(look.indexOf(lm) + lm.length, look.lastIndexOf(')"'));
const m = 'const char* kShaders = R"(';
const a = src.indexOf(m) + m.length;
const body = src.slice(a, src.indexOf(')";', a)).split(')" PRISM_LOOK_HLSL R"(').join(lookHlsl);
fs.writeFileSync('build/shaders.hlsl', body);
