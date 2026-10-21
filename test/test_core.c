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
 assert(recover_time_valid(s));
 s[56]=SYM_UNKNOWN; assert(recover_time_valid(s)); assert(!frame_valid(s));
 s[59]=SYM_ZERO;s[49]=SYM_UNKNOWN; assert(recover_time_valid(s));
 s[39]=SYM_ZERO; assert(!recover_time_valid(s));s[39]=SYM_MARK;
 s[14]=SYM_ONE;s[18]=SYM_UNKNOWN;assert(recover_time_valid(s));
 s[24]=SYM_ONE;assert(!recover_time_valid(s));s[24]=SYM_ZERO;
 s[13]=SYM_UNKNOWN;assert(!recover_time_valid(s));s[13]=SYM_ONE;
 assert(!recover_time_valid(s)); /* illegal decimal units, even in recovery */
 recovery_candidate_t a={0},b={0};a.t=51;b.t=111;a.minute=100;b.minute=101;
 assert(recovery_neighbors(&a,&b));b.minute=102;assert(!recovery_neighbors(&a,&b));
 b.minute=101;b.t=101;assert(!recovery_neighbors(&a,&b));
 recovery_candidate_t conflict[5]={{0}};
 conflict[0].t=0;conflict[0].minute=100;conflict[1].t=.01;conflict[1].minute=200;
 conflict[2].t=60;conflict[2].minute=101;conflict[3].t=60.01;conflict[3].minute=201;
 conflict[4].t=120;conflict[4].minute=102;
 recovery_confirm(conflict,5);
 for(int j=0;j<5;++j) assert(!conflict[j].confirmed);
 recovery_candidate_t pair[2]={{0}};pair[0].t=51;pair[0].minute=100;
 pair[1].t=111;pair[1].minute=101;recovery_confirm(pair,2);
 assert(pair[0].confirmed && pair[1].confirmed);
 puts("core validation passed"); return 0;
}
