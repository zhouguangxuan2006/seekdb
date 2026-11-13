#include "share/datum/ob_datum_cmp_func_def.h"
#include "common/object/ob_obj_funcs.h"
#include "share/datum/ob_datum_funcs_impl.h"
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace oceanbase::common;
static unsigned long long checks = 0;

template<bool EndSpace, bool NullFirst>
bool compare(const std::string &left, const std::string &right, bool null_left, bool null_right)
{
  ObDatum a, b;
  if (null_left) a.set_null(); else a.set_string(left.data(), left.size());
  if (null_right) b.set_null(); else b.set_string(right.data(), right.size());
  int actual = 0;
  int ret = ObNullSafeDatumStrCmp<CS_TYPE_UTF8MB4_GENERAL_CI, EndSpace, NullFirst>::cmp(a,b,actual,nullptr);
  int expected = 0;
  if (null_left || null_right) {
    expected = null_left == null_right ? 0 : null_left == NullFirst ? -1 : 1;
  } else {
    expected = ObCharset::strcmpsp(CS_TYPE_UTF8MB4_GENERAL_CI, left.data(), left.size(), right.data(), right.size(), EndSpace);
    expected = (expected > 0) - (expected < 0);
  }
  ++checks;
  if (ret || actual != expected) {
    std::fprintf(stderr,"case %llu: endspace=%d nullfirst=%d nulls=%d/%d lens=%zu/%zu actual=%d expected=%d ret=%d\n",
                 checks,EndSpace,NullFirst,null_left,null_right,left.size(),right.size(),actual,expected,ret);
    for (unsigned char c:left) std::fprintf(stderr,"%02x",c);
    std::fputc('\n',stderr);
    for (unsigned char c:right) std::fprintf(stderr,"%02x",c);
    std::fputc('\n',stderr);
    return false;
  }
  return true;
}
static bool both(const std::string &a,const std::string &b,bool na=false,bool nb=false)
{
  return compare<false,false>(a,b,na,nb) && compare<false,true>(a,b,na,nb)
      && compare<true,false>(a,b,na,nb) && compare<true,true>(a,b,na,nb);
}
int main()
{
  std::vector<std::string> tails={""," ","  ","a","A","a ","aa","Aa","prefix","PREFIX","caf\xc3\xa9","\xe4\xb8\xad","\xf0\x9f\x99\x82","\xc3","\xe4\xb8","\xff"};
  for (int c=0;c<256;++c) {
    tails.emplace_back(1,static_cast<char>(c));
    tails.push_back(std::string(" ")+char(c));
    tails.push_back(std::string(1,char(c))+" ");
  }
  for (const auto &a:tails) for (const auto &b:tails) {
    if (!both(a,b) || !both("same"+a,"SAME"+b) || !both("aA "+a,"Aa "+b)) return 1;
  }
  std::mt19937 rng(0x3927ae);
  for (int i=0;i<50000;++i) {
    std::string a,b;
    for (unsigned n=rng()%96;n;--n) a.push_back(static_cast<char>(rng()%256));
    if (i%3) { b=a; if (!b.empty()) b[rng()%b.size()]=static_cast<char>(rng()%256); }
    else for (unsigned n=rng()%96;n;--n) b.push_back(static_cast<char>(rng()%256));
    if (!both(a,b) || !both(b,a) || !both(a,b,i%7==0,i%11==0)) return 1;
  }
  std::printf("PASS %llu comparator checks against original charset implementation\n",checks);
}
