#define main decoder_program_main
#include "../wwvdecode.c"
#undef main
#include <assert.h>
int main(void) {
 const char *reference="R00001100P110001000P111000000P001000100P010000000P001000000P";
 assert(strlen(reference)==60);
 symbol_t s[60];
 for(int j=0;j<60;j++)s[j]=reference[j]=='R'?SYM_HOLE:reference[j]=='P'?SYM_MARK:reference[j]=='1'?SYM_ONE:SYM_ZERO;
 assert(frame_valid(s));
 s[13]=SYM_ONE; assert(!frame_valid(s)); s[13]=SYM_ZERO; /* BCD 11 units */
 s[14]=SYM_ONE; assert(!frame_valid(s)); s[14]=SYM_ZERO;
 s[59]=SYM_ZERO; assert(!frame_valid(s)); s[59]=SYM_MARK;
 s[56]=SYM_UNKNOWN; assert(!frame_valid(s)); s[56]=SYM_ZERO;
 s[0]=SYM_ONE; assert(!frame_valid(s)); s[0]=SYM_HOLE;
 secstat_t zero={10,1,1,1}, one={10,10,1,1}, mark={10,10,10,1}, noise={1,1,1,1};
 assert(sec_decide(&zero,3,NULL)==SYM_ZERO);
 assert(sec_decide(&one,3,NULL)==SYM_ONE);
 assert(sec_decide(&mark,3,NULL)==SYM_MARK);
 assert(sec_decide(&noise,3,NULL)==SYM_UNKNOWN);
 puts("core validation passed"); return 0;
}
