#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
/* _GNU_SOURCE (a superset of _DEFAULT_SOURCE): only for pthread_getattr_np,
 * used to learn a thread's own native stack bounds for the ASan
 * fiber-switch annotations below. */
#define _GNU_SOURCE

#include "vm.h"
#include "jit.h"
#include "bignum.h"
#include "compiler.h"
#include "disassemble.h"
#include "loader.h"
#include "prelude.h"
#include "vm_internal.h"

/* <crypt.h> exists on glibc (libxcrypt) and musl (see BCrypt.hash's own
 * comment below for what musl's version lacks), declaring crypt_r/
 * struct crypt_data/CRYPT_GENSALT_* -- but not on FreeBSD, which declares
 * plain crypt()/crypt_r() directly in <unistd.h> (already included below)
 * instead, with no separate header at all. __has_include, not an
 * __APPLE__/__FreeBSD__-style OS check (see docs/portability.md's own
 * "What hasn't been found" on why this codebase avoids those): this is a
 * feature test, and the same reasoning applies wherever else a libc omits
 * this header. */
#if __has_include(<crypt.h>)
#include <crypt.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>
#include <stdckdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <spawn.h>
#include <sqlite3.h>
#include <libpq-fe.h>
#include <mysql.h>
#include <zlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vm_internal.h"

/* Time - Time -> Float seconds; Time - Int|Float -> Time (offset
 * backward, same timezone mode). Factored out of run_chunk's own SUBTRACT/
 * MULTIPLY/DIVIDE case block for the same reason add_fallback already
 * is (see its own comment): every local declared anywhere in run_chunk's
 * switch adds to its one shared per-call stack frame, and run_chunk
 * recurses in C for every Diamond-level call -- confirmed the hard way
 * here too (an inline version of this logic reopened the exact
 * DIAMOND_MAX_CALL_DEPTH/ASan stack-overflow margin regression
 * add_fallback's comment already warns about, caught by tests/run.sh's
 * own depth(5000) case under `make sanitize`). Deliberately folds "not a
 * Time-involving mismatch" into the same DIAMOND_VM_TYPE_ERROR return as
 * a genuine error (rather than a separate `bool *matched`, the way
 * invoke_operator_method's own `bool *found` does) -- both cases want
 * exactly the same outcome here, and this keeps the run_chunk call site
 * down to one local instead of three, which is the whole point: even a
 * few bytes/variables matter at this margin (confirmed by trying the
 * three-local version first -- it still overflowed). Writes the result
 * straight into *out_result (the caller passes &registers[destination]
 * directly), so a successful call needs no separate temporary either. */
DiamondVmStatus time_subtract_fallback(DiamondVm *vm,
        DiamondValue left_value,DiamondValue right_value,DiamondValue *out_result) {
    if (left_value.kind!=DIAMOND_VALUE_OBJECT||
        left_value.as.object->kind!=DIAMOND_OBJECT_TIME)
        return DIAMOND_VM_TYPE_ERROR;
    const DiamondTime *left_time=(const DiamondTime *)left_value.as.object;
    if (right_value.kind==DIAMOND_VALUE_OBJECT&&
        right_value.as.object->kind==DIAMOND_OBJECT_TIME) {
        const DiamondTime *right_time=(const DiamondTime *)right_value.as.object;
        *out_result=DIAMOND_FLOAT(left_time->epoch-right_time->epoch);
        return DIAMOND_VM_OK;
    }
    if (right_value.kind==DIAMOND_VALUE_INT||right_value.kind==DIAMOND_VALUE_FLOAT) {
        const double offset=right_value.kind==DIAMOND_VALUE_FLOAT?
            right_value.as.real:(double)right_value.as.integer;
        DiamondTime *result=allocate_time(vm,left_time->epoch-offset,
            left_time->zone_mode,left_time->utc_offset);
        if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=DIAMOND_OBJECT(result);
        return DIAMOND_VM_OK;
    }
    return DIAMOND_VM_TYPE_ERROR;
}

bool time_comparison_fallback(DiamondValue left_value,DiamondValue right_value,
        DiamondOpCode opcode,DiamondValue *out_result) {
    if (left_value.kind!=DIAMOND_VALUE_OBJECT||
        left_value.as.object->kind!=DIAMOND_OBJECT_TIME||
        right_value.kind!=DIAMOND_VALUE_OBJECT||
        right_value.as.object->kind!=DIAMOND_OBJECT_TIME)
        return false;
    const double left_epoch=((const DiamondTime *)left_value.as.object)->epoch;
    const double right_epoch=((const DiamondTime *)right_value.as.object)->epoch;
    bool result=false;
    if(opcode==DIAMOND_OP_LESS)result=left_epoch<right_epoch;
    else if(opcode==DIAMOND_OP_LESS_EQUAL)result=left_epoch<=right_epoch;
    else if(opcode==DIAMOND_OP_GREATER)result=left_epoch>right_epoch;
    else result=left_epoch>=right_epoch;
    *out_result=DIAMOND_BOOL(result);
    return true;
}

/* Time.now()/Time.utc_now()'s shared body, and Time.at(epoch)'s --
 * pulled out of run_chunk's own TIME_NOW/TIME_AT cases for the same
 * stack-frame-budget reason as time_subtract_fallback/
 * time_comparison_fallback above (see time_subtract_fallback's own
 * comment for the full rationale, including the depth(5000)/ASan
 * regression this exact extraction fixes). */
DiamondVmStatus time_now_helper(DiamondVm *vm,bool utc,DiamondValue *out_result) {
    struct timespec now={};
    clock_gettime(CLOCK_REALTIME,&now);
    const double epoch=(double)now.tv_sec+(double)now.tv_nsec/1e9;
    DiamondTime *time=allocate_time(vm,epoch,
        utc?DIAMOND_TIME_UTC:DIAMOND_TIME_LOCAL,0);
    if(time==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(time);
    return DIAMOND_VM_OK;
}

DiamondVmStatus time_at_helper(DiamondVm *vm,DiamondValue epoch_value,
        DiamondValue *out_result) {
    if(epoch_value.kind!=DIAMOND_VALUE_INT&&epoch_value.kind!=DIAMOND_VALUE_FLOAT) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,epoch_value);
        snprintf(vm->error,sizeof vm->error,
            "Time.at epoch must be an Int or Float, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const double epoch=epoch_value.kind==DIAMOND_VALUE_FLOAT?
        epoch_value.as.real:(double)epoch_value.as.integer;
    if(isnan(epoch)||isinf(epoch)) {
        snprintf(vm->error,sizeof vm->error,"Time.at epoch must be a finite number");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondTime *time=allocate_time(vm,epoch,DIAMOND_TIME_LOCAL,0);
    if(time==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(time);
    return DIAMOND_VM_OK;
}

/* ActiveSupport-style Numeric#ago/#from_now, with the numeric receiver
 * interpreted as seconds. The returned Time is process-local, matching
 * Time.now(); callers can convert it immutably as usual. */
DiamondVmStatus time_relative_now_helper(DiamondVm *vm,DiamondValue duration_value,
        bool future,DiamondValue *out_result) {
    const double duration=duration_value.kind==DIAMOND_VALUE_FLOAT?
        duration_value.as.real:(double)duration_value.as.integer;
    if(isnan(duration)||isinf(duration)) {
        snprintf(vm->error,sizeof vm->error,
            "Time duration must be a finite number of seconds");
        return DIAMOND_VM_TYPE_ERROR;
    }
    struct timespec now={};
    clock_gettime(CLOCK_REALTIME,&now);
    const double epoch=(double)now.tv_sec+(double)now.tv_nsec/1e9+
        (future?duration:-duration);
    DiamondTime *time=allocate_time(vm,epoch,DIAMOND_TIME_LOCAL,0);
    if(time==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(time);
    return DIAMOND_VM_OK;
}

/* Breaks a Time's epoch into calendar fields. UTC and fixed offsets use
 * gmtime_r (the latter after shifting the epoch); process-local time uses
 * localtime_r and therefore remains DST-aware and system-tzdata-backed.
 * Fixed offsets never touch process-global TZ state. Whole-second resolution, same as
 * Ruby/C convention (the fractional part only matters for #to_f). Fails
 * only for a genuinely out-of-range epoch (e.g. far enough in the future
 * to overflow time_t on a 32-bit platform) -- vanishingly unlikely on
 * any 64-bit system, but checked rather than left as UB. */
bool time_struct_tm(const DiamondTime *target,struct tm *out) {
    double calendar_epoch=floor(target->epoch);
    if(target->zone_mode==DIAMOND_TIME_FIXED_OFFSET)
        calendar_epoch+=(double)target->utc_offset;
    const time_t seconds=(time_t)calendar_epoch;
    if(target->zone_mode==DIAMOND_TIME_LOCAL)
        return localtime_r(&seconds,out)!=nullptr;
    if(gmtime_r(&seconds,out)==nullptr)return false;
    if(target->zone_mode==DIAMOND_TIME_FIXED_OFFSET)out->tm_gmtoff=target->utc_offset;
    return true;
}

/* Strict ISO-8601 calendar timestamps with an explicit zone only:
 * YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM[:SS]|-HH:MM[:SS]). Parsing the calendar
 * portion through timegm keeps this independent of process-local TZ state;
 * the round trip rejects dates libc would otherwise normalize (Feb 30, etc.). */
DiamondVmStatus time_parse_helper(DiamondVm *vm,DiamondValue input,
        DiamondValue *out_result) {
    if(input.kind!=DIAMOND_VALUE_OBJECT||
       input.as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"Time.parse argument must be a String");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *string=(const DiamondString *)input.as.object;
    const char *chars=string->chars;const size_t length=string->length;
    bool valid=length>=20&&chars[4]=='-'&&chars[7]=='-'&&chars[10]=='T'&&
        chars[13]==':'&&chars[16]==':';
    const int year=valid?parse_decimal_digits(chars,0,4):-1;
    const int month=valid?parse_decimal_digits(chars,5,2):-1;
    const int day=valid?parse_decimal_digits(chars,8,2):-1;
    const int hour=valid?parse_decimal_digits(chars,11,2):-1;
    const int minute=valid?parse_decimal_digits(chars,14,2):-1;
    const int second=valid?parse_decimal_digits(chars,17,2):-1;
    /* Well-formed digits that name no real moment (month 13, hour 24,
     * February 30) are a different mistake from a malformed string, and
     * get their own message below. */
    const bool shaped=valid&&year>=0&&month>=0&&day>=0&&hour>=0&&minute>=0&&second>=0;
    valid=valid&&year>=1&&month>=1&&month<=12&&day>=1&&day<=31&&
        hour>=0&&hour<=23&&minute>=0&&minute<=59&&second>=0&&second<=59;
    size_t zone_start=19;double fraction=0.0;
    if(valid&&zone_start<length&&chars[zone_start]=='.') {
        zone_start++;const size_t fraction_start=zone_start;double scale=0.1;
        while(zone_start<length&&chars[zone_start]>='0'&&chars[zone_start]<='9') {
            fraction+=(double)(chars[zone_start]-'0')*scale;scale*=0.1;zone_start++;
        }
        if(zone_start==fraction_start)valid=false;
    }
    const bool in_range=valid;
    int32_t utc_offset=0;
    if(valid&&!parse_time_utc_offset_chars(chars+zone_start,length-zone_start,&utc_offset))
        valid=false;
    const bool zone_ok=valid;
    struct tm calendar={.tm_year=year-1900,.tm_mon=month-1,.tm_mday=day,
        .tm_hour=hour,.tm_min=minute,.tm_sec=second,.tm_isdst=0};
    const time_t calendar_epoch=valid?timegm(&calendar):(time_t)0;
    struct tm round_trip={};
    if(valid&&(gmtime_r(&calendar_epoch,&round_trip)==nullptr||
       round_trip.tm_year!=year-1900||round_trip.tm_mon!=month-1||
       round_trip.tm_mday!=day||round_trip.tm_hour!=hour||
       round_trip.tm_min!=minute||round_trip.tm_sec!=second))valid=false;
    if(!valid) {
        /* The right type with the wrong contents: ArgumentError, as for any
         * other bad value. */
        /* Digits out of range, or in range but naming no real day
         * (February 30) -- as opposed to a malformed string or zone. */
        if(shaped&&(!in_range||zone_ok)) {
            snprintf(vm->error,sizeof vm->error,
                "Time.parse: no such date or time: %.19s",chars);
            return DIAMOND_VM_ARITY_ERROR;
        }
        snprintf(vm->error,sizeof vm->error,
            "Time.parse expects YYYY-MM-DDTHH:MM:SS[.fraction](Z or signed HH:MM[:SS]), got '%.*s'",
            (int)(length<64?length:64),chars);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const bool explicit_utc=length-zone_start==1;
    DiamondTime *time=allocate_time(vm,(double)calendar_epoch-(double)utc_offset+fraction,
        explicit_utc?DIAMOND_TIME_UTC:DIAMOND_TIME_FIXED_OFFSET,utc_offset);
    if(time==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(time);return DIAMOND_VM_OK;
}

DiamondVmStatus time_build_helper(DiamondVm *vm,const DiamondValue *arguments,
        uint8_t mode,DiamondValue *out_result) {
    const bool fixed=mode==1;
    int32_t utc_offset=0;size_t field_start=0;
    if(fixed) {
        field_start=1;
        if(arguments[0].kind==DIAMOND_VALUE_INT) {
            const int64_t supplied=arguments[0].as.integer;
            if(supplied<=-86400||supplied>=86400) {
                snprintf(vm->error,sizeof vm->error,
                    "Time.fixed offset must be between -86399 and 86399 seconds");
                return DIAMOND_VM_ARITY_ERROR;
            }
            utc_offset=(int32_t)supplied;
        } else if(arguments[0].kind==DIAMOND_VALUE_OBJECT&&
                  arguments[0].as.object->kind==DIAMOND_OBJECT_STRING) {
            if(!parse_time_utc_offset(
                (const DiamondString *)arguments[0].as.object,&utc_offset)) {
                snprintf(vm->error,sizeof vm->error,
                    "Time.fixed offset must be 'Z' or a signed 'HH:MM[:SS]'");
                return DIAMOND_VM_ARITY_ERROR;
            }
        } else {
            snprintf(vm->error,sizeof vm->error,"Time.fixed offset must be an Int or String");
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    for(size_t index=0;index<6;index++) {
        if(arguments[field_start+index].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"Time calendar fields must be Int values");
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    const int64_t year=arguments[field_start].as.integer;
    const int64_t month=arguments[field_start+1].as.integer;
    const int64_t day=arguments[field_start+2].as.integer;
    const int64_t hour=arguments[field_start+3].as.integer;
    const int64_t minute=arguments[field_start+4].as.integer;
    const int64_t second=arguments[field_start+5].as.integer;
    bool valid=year>=1&&year<=9999&&month>=1&&month<=12&&day>=1&&day<=31&&
        hour>=0&&hour<=23&&minute>=0&&minute<=59&&second>=0&&second<=59;
    if(valid&&day>days_in_calendar_month((int)year,(int)month))valid=false;
    struct tm calendar={.tm_year=(int)year-1900,.tm_mon=(int)month-1,
        .tm_mday=(int)day,.tm_hour=(int)hour,.tm_min=(int)minute,
        .tm_sec=(int)second,.tm_isdst=0};
    const time_t calendar_epoch=valid?
        (mode==2?(calendar.tm_isdst=-1,mktime(&calendar)):timegm(&calendar)):(time_t)0;
    struct tm round_trip={};
    struct tm *round_trip_result=mode==2?
        localtime_r(&calendar_epoch,&round_trip):gmtime_r(&calendar_epoch,&round_trip);
    if(valid&&(round_trip_result==nullptr||(mode!=2&&(
       round_trip.tm_year!=(int)year-1900||round_trip.tm_mon!=(int)month-1||
       round_trip.tm_mday!=(int)day||round_trip.tm_hour!=(int)hour||
       round_trip.tm_min!=(int)minute||round_trip.tm_sec!=(int)second))))valid=false;
    if(!valid) {
        snprintf(vm->error,sizeof vm->error,
            "no such date or time: %04" PRId64 "-%02" PRId64 "-%02" PRId64
            " %02" PRId64 ":%02" PRId64 ":%02" PRId64,
            year,month,day,hour,minute,second);
        return DIAMOND_VM_ARITY_ERROR;
    }
    DiamondTime *time=allocate_time(vm,(double)calendar_epoch-
        (fixed?(double)utc_offset:0.0),
        fixed?DIAMOND_TIME_FIXED_OFFSET:
            (mode==2?DIAMOND_TIME_LOCAL:DIAMOND_TIME_UTC),utc_offset);
    if(time==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(time);return DIAMOND_VM_OK;
}

/* Calendar-aware movement. Unlike numeric day/week durations, this preserves
 * wall-clock fields in the receiver's display zone; month/year shifts also
 * clamp the day to the target month's end. Local mode delegates DST resolution
 * to mktime; UTC/fixed modes remain process-state-independent through timegm. */
static DiamondVmStatus time_calendar_shift_helper(DiamondVm *vm,
        const DiamondTime *target,int64_t amount,uint8_t unit,bool future,
        DiamondValue *out_result) {
    int64_t limit=120000;
    if(unit==TIME_SHIFT_DAYS)limit=3660000;
    else if(unit==TIME_SHIFT_WEEKS)limit=522000;
    else if(unit==TIME_SHIFT_YEARS)limit=10000;
    if(amount < -limit||amount > limit) {
        snprintf(vm->error,sizeof vm->error,"Time calendar shift is out of range");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t delta=future?amount:-amount;
    struct tm parts;
    if(!time_struct_tm(target,&parts)) {
        snprintf(vm->error,sizeof vm->error,"Time value out of range");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(unit==TIME_SHIFT_DAYS||unit==TIME_SHIFT_WEEKS) {
        parts.tm_mday+=(int)(delta*(unit==TIME_SHIFT_WEEKS?7:1));
        time_t shifted_epoch;
        if(target->zone_mode==DIAMOND_TIME_LOCAL) {
            parts.tm_isdst=-1;shifted_epoch=mktime(&parts);
        } else {
            shifted_epoch=timegm(&parts);
            if(target->zone_mode==DIAMOND_TIME_FIXED_OFFSET)
                shifted_epoch-=(time_t)target->utc_offset;
        }
        const double fraction=target->epoch-floor(target->epoch);
        const DiamondTime shifted={.epoch=(double)shifted_epoch+fraction,
            .utc_offset=target->utc_offset,.zone_mode=target->zone_mode};
        struct tm check;
        if(!time_struct_tm(&shifted,&check)||check.tm_year+1900<1||
           check.tm_year+1900>9999) {
            snprintf(vm->error,sizeof vm->error,"Time calendar shift is out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondTime *result=allocate_time(vm,shifted.epoch,target->zone_mode,
            target->utc_offset);
        if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=DIAMOND_OBJECT(result);return DIAMOND_VM_OK;
    }
    int64_t shifted_year=(int64_t)parts.tm_year+1900;
    int shifted_month=parts.tm_mon+1;
    if(unit==TIME_SHIFT_YEARS)shifted_year+=delta;
    else {
        const int64_t total=shifted_year*12+(shifted_month-1)+delta;
        if(total<12||total>9999*12+11) {
            snprintf(vm->error,sizeof vm->error,"Time calendar shift is out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        shifted_year=total/12;shifted_month=(int)(total%12)+1;
    }
    if(shifted_year<1||shifted_year>9999) {
        snprintf(vm->error,sizeof vm->error,"Time calendar shift is out of range");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int last_day=days_in_calendar_month((int)shifted_year,shifted_month);
    if(parts.tm_mday>last_day)parts.tm_mday=last_day;
    parts.tm_year=(int)shifted_year-1900;parts.tm_mon=shifted_month-1;
    time_t shifted_epoch;
    if(target->zone_mode==DIAMOND_TIME_LOCAL) {
        parts.tm_isdst=-1;shifted_epoch=mktime(&parts);
    } else {
        shifted_epoch=timegm(&parts);
        if(target->zone_mode==DIAMOND_TIME_FIXED_OFFSET)
            shifted_epoch-=(time_t)target->utc_offset;
    }
    const double fraction=target->epoch-floor(target->epoch);
    DiamondTime *result=allocate_time(vm,(double)shifted_epoch+fraction,
        target->zone_mode,target->utc_offset);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);return DIAMOND_VM_OK;
}

/* Wall-clock period boundaries in the receiver's own display zone. End
 * boundaries are the final microsecond before the following boundary, so a
 * local day naturally follows libc's 23/24/25-hour DST transition rules. */
static DiamondVmStatus time_boundary_helper(DiamondVm *vm,const DiamondTime *target,
        uint8_t boundary,DiamondValue *out_result) {
    struct tm parts;
    if(!time_struct_tm(target,&parts)) {
        snprintf(vm->error,sizeof vm->error,"Time value out of range");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const bool end=boundary==TIME_END_OF_DAY||boundary==TIME_END_OF_MONTH||
        boundary==TIME_END_OF_WEEK||boundary==TIME_END_OF_YEAR||
        boundary==TIME_END_OF_QUARTER;
    parts.tm_hour=0;parts.tm_min=0;parts.tm_sec=0;
    if(boundary==TIME_END_OF_DAY)parts.tm_mday++;
    else if(boundary==TIME_BEGINNING_OF_MONTH)parts.tm_mday=1;
    else if(boundary==TIME_END_OF_MONTH) { parts.tm_mday=1;parts.tm_mon++; }
    else if(boundary==TIME_BEGINNING_OF_WEEK)
        parts.tm_mday-=(parts.tm_wday+6)%7;
    else if(boundary==TIME_END_OF_WEEK)
        parts.tm_mday+=7-(parts.tm_wday+6)%7;
    else if(boundary==TIME_BEGINNING_OF_YEAR) { parts.tm_mon=0;parts.tm_mday=1; }
    else if(boundary==TIME_END_OF_YEAR) {
        parts.tm_year++;parts.tm_mon=0;parts.tm_mday=1;
    } else if(boundary==TIME_BEGINNING_OF_QUARTER) {
        parts.tm_mon=(parts.tm_mon/3)*3;parts.tm_mday=1;
    } else if(boundary==TIME_END_OF_QUARTER) {
        parts.tm_mon=(parts.tm_mon/3)*3+3;parts.tm_mday=1;
    }
    time_t epoch;
    if(target->zone_mode==DIAMOND_TIME_LOCAL) {
        parts.tm_isdst=-1;epoch=mktime(&parts);
    } else {
        epoch=timegm(&parts);
        if(target->zone_mode==DIAMOND_TIME_FIXED_OFFSET)
            epoch-=(time_t)target->utc_offset;
    }
    DiamondTime *result=allocate_time(vm,(double)epoch-(end?0.000001:0.0),
        target->zone_mode,target->utc_offset);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);return DIAMOND_VM_OK;
}

/* #year/#month/#day/#hour/#min/#sec/#wday/#yday/#to_i/#to_f/#strftime/
 * #iso8601/calendar shifts/#to_s/#utc/#localtime/#utc? -- factored out of
 * the INVOKE case body
 * for the same stack-frame reason sqlite3_dispatch_helper's own comment
 * explains (immediately above). */
DiamondVmStatus time_dispatch_helper(DiamondVm *vm,DiamondTime *target,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    bool component=false;int component_value=0;
    struct tm parts={};
    const bool needs_parts=
        (method_name->length==4&&memcmp(method_name->chars,"year",4)==0)||
        (method_name->length==5&&memcmp(method_name->chars,"month",5)==0)||
        (method_name->length==3&&memcmp(method_name->chars,"day",3)==0)||
        (method_name->length==4&&memcmp(method_name->chars,"hour",4)==0)||
        (method_name->length==3&&memcmp(method_name->chars,"min",3)==0)||
        (method_name->length==3&&memcmp(method_name->chars,"sec",3)==0)||
        (method_name->length==4&&memcmp(method_name->chars,"wday",4)==0)||
        (method_name->length==4&&memcmp(method_name->chars,"yday",4)==0);
    if(needs_parts) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(!time_struct_tm(target,&parts)) {
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        component=true;
        if(method_name->length==4&&memcmp(method_name->chars,"year",4)==0)
            component_value=parts.tm_year+1900;
        else if(method_name->length==5&&memcmp(method_name->chars,"month",5)==0)
            component_value=parts.tm_mon+1;
        else if(method_name->length==3&&memcmp(method_name->chars,"day",3)==0)
            component_value=parts.tm_mday;
        else if(method_name->length==4&&memcmp(method_name->chars,"hour",4)==0)
            component_value=parts.tm_hour;
        else if(method_name->length==3&&memcmp(method_name->chars,"min",3)==0)
            component_value=parts.tm_min;
        else if(method_name->length==3&&memcmp(method_name->chars,"sec",3)==0)
            component_value=parts.tm_sec;
        else if(method_name->length==4&&memcmp(method_name->chars,"wday",4)==0)
            component_value=parts.tm_wday;
        else component_value=parts.tm_yday+1;
    }
    if(component) {
        registers[dest]=DIAMOND_INT(component_value);
        return DIAMOND_VM_OK;
    }
    const bool today_p_method=method_name->length==6&&
        memcmp(method_name->chars,"today?",6)==0;
    const bool yesterday_p_method=method_name->length==10&&
        memcmp(method_name->chars,"yesterday?",10)==0;
    const bool tomorrow_p_method=method_name->length==9&&
        memcmp(method_name->chars,"tomorrow?",9)==0;
    const bool same_day_p_method=method_name->length==9&&
        memcmp(method_name->chars,"same_day?",9)==0;
    const bool past_p_method=method_name->length==5&&
        memcmp(method_name->chars,"past?",5)==0;
    const bool future_p_method=method_name->length==7&&
        memcmp(method_name->chars,"future?",7)==0;
    const bool weekend_p_method=method_name->length==11&&
        memcmp(method_name->chars,"on_weekend?",11)==0;
    const bool weekday_p_method=method_name->length==11&&
        memcmp(method_name->chars,"on_weekday?",11)==0;
    if(same_day_p_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_TIME) {
            snprintf(vm->error,sizeof vm->error,"Time#same_day? argument must be a Time");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondTime *other=(const DiamondTime *)registers[base].as.object;
        const DiamondTime projected={.epoch=other->epoch,
            .utc_offset=target->utc_offset,.zone_mode=target->zone_mode};
        struct tm other_parts;
        if(!time_struct_tm(target,&parts)||!time_struct_tm(&projected,&other_parts)) {
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        registers[dest]=DIAMOND_BOOL(parts.tm_year==other_parts.tm_year&&
            parts.tm_yday==other_parts.tm_yday);
        return DIAMOND_VM_OK;
    }
    if(today_p_method||yesterday_p_method||tomorrow_p_method||
       past_p_method||future_p_method||weekend_p_method||
       weekday_p_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(weekend_p_method||weekday_p_method) {
            if(!time_struct_tm(target,&parts)) {
                snprintf(vm->error,sizeof vm->error,"Time value out of range");
                return DIAMOND_VM_TYPE_ERROR;
            }
            const bool weekend=parts.tm_wday==0||parts.tm_wday==6;
            registers[dest]=DIAMOND_BOOL(weekend_p_method?weekend:!weekend);
            return DIAMOND_VM_OK;
        }
        struct timespec now={};clock_gettime(CLOCK_REALTIME,&now);
        const double now_epoch=(double)now.tv_sec+(double)now.tv_nsec/1e9;
        if(past_p_method||future_p_method) {
            registers[dest]=DIAMOND_BOOL(past_p_method?
                target->epoch<now_epoch:target->epoch>now_epoch);
            return DIAMOND_VM_OK;
        }
        const DiamondTime current={.epoch=now_epoch,
            .utc_offset=target->utc_offset,.zone_mode=target->zone_mode};
        struct tm current_parts;
        if(!time_struct_tm(target,&parts)||!time_struct_tm(&current,&current_parts)) {
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        int expected_year=current_parts.tm_year;
        int expected_yday=current_parts.tm_yday;
        if(yesterday_p_method) {
            expected_yday--;
            if(expected_yday<0) {
                expected_year--;
                const int year=expected_year+1900;
                expected_yday=((year%4==0&&year%100!=0)||year%400==0)?365:364;
            }
        } else if(tomorrow_p_method) {
            expected_yday++;
            const int year=current_parts.tm_year+1900;
            const int days=((year%4==0&&year%100!=0)||year%400==0)?366:365;
            if(expected_yday==days) { expected_year++;expected_yday=0; }
        }
        registers[dest]=DIAMOND_BOOL(parts.tm_year==expected_year&&
            parts.tm_yday==expected_yday);
        return DIAMOND_VM_OK;
    }
    const bool to_i_method=method_name->length==4&&
        memcmp(method_name->chars,"to_i",4)==0;
    const bool to_f_method=method_name->length==4&&
        memcmp(method_name->chars,"to_f",4)==0;
    if(to_i_method||to_f_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=to_i_method?
            DIAMOND_INT((int64_t)target->epoch):DIAMOND_FLOAT(target->epoch);
        return DIAMOND_VM_OK;
    }
    const bool utc_p_method=method_name->length==4&&
        memcmp(method_name->chars,"utc?",4)==0;
    if(utc_p_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_BOOL(target->zone_mode==DIAMOND_TIME_UTC);
        return DIAMOND_VM_OK;
    }
    const bool utc_offset_method=method_name->length==10&&
        memcmp(method_name->chars,"utc_offset",10)==0;
    if(utc_offset_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(target->zone_mode==DIAMOND_TIME_UTC)component_value=0;
        else if(target->zone_mode==DIAMOND_TIME_FIXED_OFFSET)
            component_value=target->utc_offset;
        else {
            if(!time_struct_tm(target,&parts)) {
                snprintf(vm->error,sizeof vm->error,"Time value out of range");
                return DIAMOND_VM_TYPE_ERROR;
            }
            component_value=(int)parts.tm_gmtoff;
        }
        registers[dest]=DIAMOND_INT(component_value);
        return DIAMOND_VM_OK;
    }
    const bool utc_method=method_name->length==3&&
        memcmp(method_name->chars,"utc",3)==0;
    const bool localtime_method=method_name->length==9&&
        memcmp(method_name->chars,"localtime",9)==0;
    if(utc_method||localtime_method) {
        if(utc_method&&argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(localtime_method&&argc>1)return DIAMOND_VM_ARITY_ERROR;
        uint8_t zone_mode=utc_method?DIAMOND_TIME_UTC:DIAMOND_TIME_LOCAL;
        int32_t utc_offset=0;
        if(localtime_method&&argc==1) {
            if(registers[base].kind==DIAMOND_VALUE_OBJECT&&
               registers[base].as.object->kind==DIAMOND_OBJECT_STRING) {
                if(!parse_time_utc_offset(
                    (const DiamondString *)registers[base].as.object,&utc_offset)) {
                    snprintf(vm->error,sizeof vm->error,
                        "Time#localtime offset must be 'Z' or a signed 'HH:MM[:SS]'");
                    return DIAMOND_VM_ARITY_ERROR;
                }
            } else if(registers[base].kind==DIAMOND_VALUE_INT) {
                const int64_t supplied=registers[base].as.integer;
                if(supplied<=-86400||supplied>=86400) {
                    snprintf(vm->error,sizeof vm->error,
                        "Time#localtime offset must be between -86399 and 86399 seconds");
                    return DIAMOND_VM_ARITY_ERROR;
                }
                utc_offset=(int32_t)supplied;
            } else {
                snprintf(vm->error,sizeof vm->error,
                    "Time#localtime offset must be an Int or String");
                return DIAMOND_VM_TYPE_ERROR;
            }
            zone_mode=DIAMOND_TIME_FIXED_OFFSET;
        }
        DiamondTime *copy=allocate_time(vm,target->epoch,zone_mode,utc_offset);
        if(copy==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(copy);
        return DIAMOND_VM_OK;
    }
    const bool months_ago_method=method_name->length==10&&
        memcmp(method_name->chars,"months_ago",10)==0;
    const bool months_from_now_method=method_name->length==15&&
        memcmp(method_name->chars,"months_from_now",15)==0;
    const bool years_ago_method=method_name->length==9&&
        memcmp(method_name->chars,"years_ago",9)==0;
    const bool years_from_now_method=method_name->length==14&&
        memcmp(method_name->chars,"years_from_now",14)==0;
    const bool days_ago_method=method_name->length==8&&
        memcmp(method_name->chars,"days_ago",8)==0;
    const bool days_from_now_method=method_name->length==13&&
        memcmp(method_name->chars,"days_from_now",13)==0;
    const bool weeks_ago_method=method_name->length==9&&
        memcmp(method_name->chars,"weeks_ago",9)==0;
    const bool weeks_from_now_method=method_name->length==14&&
        memcmp(method_name->chars,"weeks_from_now",14)==0;
    if(days_ago_method||days_from_now_method||weeks_ago_method||
       weeks_from_now_method||months_ago_method||months_from_now_method||
       years_ago_method||years_from_now_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "Time calendar shift amount must be an Int");
            return DIAMOND_VM_TYPE_ERROR;
        }
        uint8_t unit=TIME_SHIFT_MONTHS;
        if(days_ago_method||days_from_now_method)unit=TIME_SHIFT_DAYS;
        else if(weeks_ago_method||weeks_from_now_method)unit=TIME_SHIFT_WEEKS;
        else if(years_ago_method||years_from_now_method)unit=TIME_SHIFT_YEARS;
        return time_calendar_shift_helper(vm,target,registers[base].as.integer,unit,
            days_from_now_method||weeks_from_now_method||months_from_now_method||
                years_from_now_method,&registers[dest]);
    }
    const bool next_weekday_method=method_name->length==12&&
        memcmp(method_name->chars,"next_weekday",12)==0;
    const bool previous_weekday_method=method_name->length==16&&
        memcmp(method_name->chars,"previous_weekday",16)==0;
    if(next_weekday_method||previous_weekday_method) {
        if(argc>1)return DIAMOND_VM_ARITY_ERROR;
        int64_t count=1;
        if(argc==1) {
            if(registers[base].kind!=DIAMOND_VALUE_INT) {
                snprintf(vm->error,sizeof vm->error,
                    "Time weekday navigation count must be an Int");
                return DIAMOND_VM_TYPE_ERROR;
            }
            count=registers[base].as.integer;
            if(count<0||count>2610000) {
                snprintf(vm->error,sizeof vm->error,
                    "Time weekday navigation count must be between 0 and 2610000");
                return DIAMOND_VM_TYPE_ERROR;
            }
        }
        if(count==0) { registers[dest]=DIAMOND_OBJECT(target);return DIAMOND_VM_OK; }
        if(!time_struct_tm(target,&parts)) {
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t days=weekday_calendar_distance(parts.tm_wday,count,
            next_weekday_method);
        return time_calendar_shift_helper(vm,target,days,TIME_SHIFT_DAYS,
            next_weekday_method,&registers[dest]);
    }
    const bool beginning_of_day_method=method_name->length==16&&
        memcmp(method_name->chars,"beginning_of_day",16)==0;
    const bool end_of_day_method=method_name->length==10&&
        memcmp(method_name->chars,"end_of_day",10)==0;
    const bool beginning_of_month_method=method_name->length==18&&
        memcmp(method_name->chars,"beginning_of_month",18)==0;
    const bool end_of_month_method=method_name->length==12&&
        memcmp(method_name->chars,"end_of_month",12)==0;
    const bool beginning_of_week_method=method_name->length==17&&
        memcmp(method_name->chars,"beginning_of_week",17)==0;
    const bool end_of_week_method=method_name->length==11&&
        memcmp(method_name->chars,"end_of_week",11)==0;
    const bool beginning_of_year_method=method_name->length==17&&
        memcmp(method_name->chars,"beginning_of_year",17)==0;
    const bool end_of_year_method=method_name->length==11&&
        memcmp(method_name->chars,"end_of_year",11)==0;
    const bool beginning_of_quarter_method=method_name->length==20&&
        memcmp(method_name->chars,"beginning_of_quarter",20)==0;
    const bool end_of_quarter_method=method_name->length==14&&
        memcmp(method_name->chars,"end_of_quarter",14)==0;
    if(beginning_of_day_method||end_of_day_method||beginning_of_month_method||
       end_of_month_method||beginning_of_week_method||end_of_week_method||
       beginning_of_year_method||end_of_year_method||
       beginning_of_quarter_method||end_of_quarter_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        uint8_t boundary=TIME_BEGINNING_OF_DAY;
        if(end_of_day_method)boundary=TIME_END_OF_DAY;
        else if(beginning_of_month_method)boundary=TIME_BEGINNING_OF_MONTH;
        else if(end_of_month_method)boundary=TIME_END_OF_MONTH;
        else if(beginning_of_week_method)boundary=TIME_BEGINNING_OF_WEEK;
        else if(end_of_week_method)boundary=TIME_END_OF_WEEK;
        else if(beginning_of_year_method)boundary=TIME_BEGINNING_OF_YEAR;
        else if(end_of_year_method)boundary=TIME_END_OF_YEAR;
        else if(beginning_of_quarter_method)boundary=TIME_BEGINNING_OF_QUARTER;
        else if(end_of_quarter_method)boundary=TIME_END_OF_QUARTER;
        return time_boundary_helper(vm,target,boundary,&registers[dest]);
    }
    const bool to_s_method=method_name->length==4&&
        memcmp(method_name->chars,"to_s",4)==0;
    if(to_s_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        StringBuilder builder={};
        if(!format_time_default(target,&builder)) {
            free(builder.chars);
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondString *string=allocate_string(vm,builder.chars,builder.length);
        free(builder.chars);
        if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(string);
        return DIAMOND_VM_OK;
    }
    const bool iso8601_method=method_name->length==7&&
        memcmp(method_name->chars,"iso8601",7)==0;
    if(iso8601_method) {
        if(argc>1)return DIAMOND_VM_ARITY_ERROR;
        int precision=0;
        if(argc==1) {
            if(registers[base].kind!=DIAMOND_VALUE_INT) {
                snprintf(vm->error,sizeof vm->error,
                    "Time#iso8601 precision must be an Int");
                return DIAMOND_VM_TYPE_ERROR;
            }
            const int64_t supplied=registers[base].as.integer;
            if(supplied<0||supplied>9) {
                snprintf(vm->error,sizeof vm->error,
                    "Time#iso8601 precision must be between 0 and 9");
                return DIAMOND_VM_TYPE_ERROR;
            }
            precision=(int)supplied;
        }
        StringBuilder builder={};
        if(!format_time_iso8601(target,precision,&builder)) {
            free(builder.chars);
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondString *string=allocate_string(vm,builder.chars,builder.length);
        free(builder.chars);
        if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
    }
    const bool strftime_method=method_name->length==8&&
        memcmp(method_name->chars,"strftime",8)==0;
    if(strftime_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "Time#strftime argument must be a String");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *format=(const DiamondString *)registers[base].as.object;
        if(!time_struct_tm(target,&parts)) {
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const char *format_chars=format->chars;
        char *substituted=nullptr;
        if(target->zone_mode==DIAMOND_TIME_FIXED_OFFSET) {
            substituted=substitute_fixed_offset_z(format_chars,target->utc_offset);
            if(substituted==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
            format_chars=substituted;
        }
        char stack_buffer[256];
        size_t length=strftime(stack_buffer,sizeof stack_buffer,format_chars,&parts);
        const char *result_chars=stack_buffer;
        char *heap_buffer=nullptr;
        if(length==0) {
            heap_buffer=malloc(4096);
            if(heap_buffer==nullptr){free(substituted);return DIAMOND_VM_OUT_OF_MEMORY;}
            length=strftime(heap_buffer,4096,format_chars,&parts);
            result_chars=heap_buffer;
        }
        DiamondString *string=allocate_string(vm,result_chars,length);
        free(heap_buffer);
        free(substituted);
        if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(string);
        return DIAMOND_VM_OK;
    }
    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
        (int)method_name->length,method_name->chars,"Time");
    return DIAMOND_VM_TYPE_ERROR;
}
