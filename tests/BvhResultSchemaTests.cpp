#include "graphics/shader/recompiler/BvhDiagnosticRecord.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>
namespace D=Libs::Graphics::ShaderRecompiler::Diagnostics;
void Check(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
int main(){try{
 std::array<uint32_t,D::BvhResultDiagnosticWords> words {};
 words[0]=1;words[1]=8;words[12]=0x25;words[15]=0x80000000;
 words[D::BvhNodeWidthWord]=1;words[D::BvhNativeLaneWord]=63;
 words[D::BvhSchemaMagicWord]=D::BvhSchemaMagic;words[D::BvhSchemaVersionWord]=2;
 words[30]=0x12345678;words[31]=0xffffffff;words[32]=0;words[33]=0x80000000;
 Check(D::BvhResultDiagnosticWords==34&&D::BvhResultDiagnosticExtraWords==26&&sizeof(words)==136,"new schema extent");
 Check(D::ValidBvhResultSidecar(words),"complete upper-lane raw results rejected");
 Check(!D::ValidBvhRaySidecar(words),"kind8 accepted by old schema");
 Check(!D::ValidBvhResultSidecar(std::span(words).first(33)),"short new schema accepted");
 const auto original=words;
 for(auto [slot,value]:std::array<std::pair<size_t,uint32_t>,10>{{{0,0},{0,2},{1,7},{1,3},{26,0},{26,3},{27,64},{28,0},{29,1},{13,1}}}){
   words=original;words[slot]=value;Check(!D::ValidBvhResultSidecar(words),"invalid schema metadata accepted");
 }
 words=original;words[15]=0;Check(!D::ValidBvhResultSidecar(words),"inactive selected upper lane accepted");
 words=original;words[26]=2;words[13]=0xabcdef01;Check(D::ValidBvhResultSidecar(words),"wide raw node rejected");
 words=original;words[14]=1;words[15]=0;words[27]=0;Check(D::ValidBvhResultSidecar(words),"active low lane rejected");
 std::array<uint32_t,32> old {};old[0]=1;old[1]=7;old[14]=1;old[26]=1;old[27]=0;old[28]=D::BvhSchemaMagic;old[29]=1;
 Check(D::BvhDiagnosticWords==32&&D::BvhDiagnosticExtraWords==24&&D::ValidBvhRaySidecar(old),"old128-byte schema changed");
 Check(!D::ValidBvhResultSidecar(old),"old kind7 accepted by result validator");
 std::array<uint32_t,35> long_words {};std::copy(original.begin(),original.end(),long_words.begin());
 Check(!D::ValidBvhResultSidecar(long_words),"long record accepted");
 std::puts("PASS exact136B kind8/schema2 arbitrary-results and active lane metadata");
 std::puts("PASS invalid extent/kind/version/width/selected-lane rejection");
 std::puts("PASS old128B kind7/schema1 unchanged; host address/native-PC validation remains separate");
 return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
