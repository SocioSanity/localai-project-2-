#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ID_PROMPT 101
#define ID_SEND 102
#define ID_LOG 103
#define ID_SEARCH 104
#define ID_FIND 105
#define ID_STATUS 106
#define WM_APPEND_TEXT (WM_APP + 1)
#define WM_REQUEST_DONE (WM_APP + 2)
#define WM_INFERENCE_PROGRESS (WM_APP + 3)

static HWND prompt_edit, output_box, send_button, search_edit, status_label;
static HFONT ui_font;
static size_t last_find_position;
#if defined(APOLLO_COMPACT)
#define INFER_EXE "apollo_compact_infer.exe"
#define MODEL_LABEL L"Apollo LLM 1.0 Compact"
#define WINDOW_TITLE L"Apollo LLM 1.0 Compact Chat"
static const wchar_t *model_path = L"model\\apollo-llm-1.0-compact.gguf";
#else
#define INFER_EXE "apollo_1_0_infer.exe"
#define MODEL_LABEL L"Apollo LLM 1.0"
#define WINDOW_TITLE L"Apollo LLM 1.0 Chat"
static const wchar_t *model_path = L"model\\apollo-llm-1.0-compact.gguf";
static const wchar_t *language_model_path = L"model\\apollo-llm-1.0-language.gguf";
#endif
static char *conversation;
static wchar_t memory_path[MAX_PATH];
static wchar_t reference_path[MAX_PATH];
static int conversation_restored;

static const char system_message[] =
    "<|im_start|>system\n"
    "You are Apollo LLM 1.0, a thoughtful conversational assistant and capable C programmer. "
    "Answer the user's actual point directly; avoid filler and repeating the question. "
    "For general questions, explain the main idea with concrete examples and say when uncertain. "
    "Do not guess precise numbers or dates when unsure. "
    "For complex questions, organize the answer around the most relevant facts and give a clear conclusion. "
    "For a large or multi-part problem, decompose it into a few small steps, solve and check each step, then combine the results. "
    "Show the steps when they help the user follow the work; when a direct result is clearer and takes less work, give the concise result and only the key checks. "
    "Do not pad simple answers with unnecessary steps. "
    "For C requests, provide correct, complete code that compiles, then explain how to use it briefly. "
    "Ask follow-ups only when needed.\n"
    "<|im_end|>\n";

static wchar_t *utf8_to_wide(const char *s);

typedef struct {
    HWND window;
    char *prompt_utf8;
    unsigned min_tokens, max_tokens;
    ULONGLONG deadline_tick;
    volatile LONG stage;
} Request;
typedef struct { char *context; char *text; } Response;

static int contains_ascii_ci(const char *text,const char *word)
{
    for (const char *p=text; *p; ++p) {
        const char *a=p,*b=word;
        while (*a && *b && tolower((unsigned char)*a)==tolower((unsigned char)*b)) { ++a; ++b; }
        if (!*b) return 1;
    }
    return 0;
}

static unsigned min_reply_tokens(const char *message)
{
    static const char *signals[]={
        "explain","discuss","compare","analyze","analyse","reason",
        "pros and cons","write","implement",
        "function","debug","code","how does","how do","how to","should","why",
        "solve","calculate","perimeter","dimension","equation","what causes",
        "evaluate","step by step","break down","multi-step","multi step",
        "work through","derive","prove","large problem",
        "complex problem","reasoning","plan the steps"
    };
    for (size_t i=0;i<sizeof(signals)/sizeof(signals[0]);++i)
        if (contains_ascii_ci(message,signals[i])) return 24;
    return strlen(message)>140 ? 24 : 0;
}

static int message_needs_decomposition(const char *message)
{
    static const char *signals[]={
        "step by step","break down","multi-step","multi step","work through",
        "derive","prove","analyze","analyse","large problem","complex problem",
        "reason through","plan the steps","evaluate","compare"
    };
    for (size_t i=0;i<sizeof(signals)/sizeof(signals[0]);++i)
        if (contains_ascii_ci(message,signals[i])) return 1;
    return 0;
}

static int message_needs_code_model(const char *message)
{
    static const char *signals[]={
        "code","program","programming","compiler","compile","debug",
        "implement","source","header","pointer","array","syntax","segfault",
        "algorithm","makefile","gcc","api","bug","in c","c code","c program",
        "write a function","implement a function","c function","function in c"
    };
    for (size_t i=0;i<sizeof(signals)/sizeof(signals[0]);++i)
        if (contains_ascii_ci(message,signals[i])) return 1;
    return 0;
}

static void append_wide(const wchar_t *s)
{
    int n = GetWindowTextLengthW(output_box);
    SendMessageW(output_box, EM_SETSEL, n, n);
    SendMessageW(output_box, EM_REPLACESEL, FALSE, (LPARAM)s);
}

static void post_status(Request *req,const wchar_t *label,size_t output_bytes)
{
    wchar_t text[160];
    if (output_bytes)
        _snwprintf(text,159,L"%ls  •  %zu bytes generated",label,output_bytes);
    else
        _snwprintf(text,159,L"%ls",label);
    text[159]=0;
    size_t n=wcslen(text)+1;
    wchar_t *copy=malloc(n*sizeof(wchar_t));
    if (!copy) return;
    memcpy(copy,text,n*sizeof(wchar_t));
    if (!PostMessageW(req->window,WM_INFERENCE_PROGRESS,0,(LPARAM)copy)) free(copy);
}

static void apply_ui_font(HWND control)
{
    if (control && ui_font) SendMessageW(control,WM_SETFONT,(WPARAM)ui_font,TRUE);
}

static void find_in_chat(HWND window)
{
    int query_len=GetWindowTextLengthW(search_edit);
    int text_len=GetWindowTextLengthW(output_box);
    if (query_len<=0 || text_len<=0) {
        SetWindowTextW(status_label,L"Type a word or phrase to search this chat.");
        return;
    }
    wchar_t *query=malloc(((size_t)query_len+1)*sizeof(wchar_t));
    wchar_t *text=malloc(((size_t)text_len+1)*sizeof(wchar_t));
    if (!query || !text) { free(query); free(text); return; }
    GetWindowTextW(search_edit,query,query_len+1);
    GetWindowTextW(output_box,text,text_len+1);
    size_t q=(size_t)query_len,n=(size_t)text_len,found=SIZE_MAX;
    size_t start=last_find_position<n?last_find_position:0;
    for (size_t i=start;i+q<=n;++i)
        if (!_wcsnicmp(text+i,query,q)) { found=i; break; }
    if (found==SIZE_MAX && start)
        for (size_t i=0;i<start && i+q<=n;++i)
            if (!_wcsnicmp(text+i,query,q)) { found=i; break; }
    if (found!=SIZE_MAX) {
        SendMessageW(output_box,EM_SETSEL,(WPARAM)found,(LPARAM)(found+q));
        SendMessageW(output_box,EM_SCROLLCARET,0,0);
        SetFocus(output_box);
        last_find_position=found+q;
        SetWindowTextW(status_label,L"Match found. Press Find again to continue.");
    } else {
        last_find_position=0;
        SetWindowTextW(status_label,L"No match in this conversation.");
    }
    free(query); free(text); (void)window;
}

static void init_memory_path(void)
{
    wchar_t module[MAX_PATH];
    DWORD n=GetModuleFileNameW(NULL,module,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    wchar_t *slash=wcsrchr(module,L'\\');
    if (!slash) return;
    slash[1]=L'\0';
    if (wcslen(module)+wcslen(L"Apollo_1.0.memory")>=MAX_PATH) return;
    wcscpy(memory_path,module);
    wcscat(memory_path,L"Apollo_1.0.memory");
    if (wcslen(module)+wcslen(L"Apollo_1.0.references")<MAX_PATH) {
        wcscpy(reference_path,module);
        wcscat(reference_path,L"Apollo_1.0.references");
    }
}

static char *latest_user_text(const char *prompt)
{
    static const char tag[]="<|im_start|>user\n";
    const char *last=NULL,*scan=prompt;
    while ((scan=strstr(scan,tag))!=NULL) { last=scan+sizeof(tag)-1; scan=last; }
    if (!last) return _strdup("");
    const char *end=strstr(last,"<|im_end|>");
    if (!end) end=last+strlen(last);
    while (last<end && (*last==' '||*last=='\t'||*last=='\r'||*last=='\n')) ++last;
    while (end>last && (end[-1]==' '||end[-1]=='\t'||end[-1]=='\r'||end[-1]=='\n')) --end;
    size_t n=(size_t)(end-last);
    char *text=malloc(n+1);
    if (text) { memcpy(text,last,n); text[n]=0; }
    return text;
}

static char *normalize_question(const char *question)
{
    size_t n=strlen(question),out=0;
    char *key=malloc(n+1);
    if (!key) return NULL;
    int pending_space=0;
    for (size_t i=0;i<n;++i) {
        unsigned char c=(unsigned char)question[i];
        if (isspace(c)) { pending_space=out>0; continue; }
        if (ispunct(c) && c<128) continue;
        if (pending_space) key[out++]=' ';
        pending_space=0;
        key[out++]=(char)tolower(c);
    }
    key[out]=0;
    return key;
}

static char *find_reference(const char *question)
{
    if (!reference_path[0] || !question || !*question) return NULL;
    HANDLE file=CreateFileW(reference_path,GENERIC_READ,FILE_SHARE_READ,NULL,
                            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file,&size) || size.QuadPart<=0 || size.QuadPart>16*1024*1024) {
        CloseHandle(file); return NULL;
    }
    char *data=malloc((size_t)size.QuadPart+1);
    DWORD got=0;
    if (!data || !ReadFile(file,data,(DWORD)size.QuadPart,&got,NULL) || got!=(DWORD)size.QuadPart) {
        free(data); CloseHandle(file); return NULL;
    }
    data[got]=0;
    CloseHandle(file);
    char *answer=NULL;
    const char *p=data,*limit=data+got;
    while (p<limit) {
        char *end=NULL;
        unsigned long long qn=strtoull(p,&end,10);
        if (end==p || end>=limit || *end!=' ') break;
        p=end+1;
        unsigned long long an=strtoull(p,&end,10);
        if (end==p || end>=limit || *end!='\n') break;
        p=end+1;
        if (qn>(unsigned long long)(limit-p) || an>(unsigned long long)(limit-p)-(size_t)qn ||
            p+(size_t)qn+(size_t)an>=limit) break;
        if (strlen(question)==(size_t)qn && !memcmp(p,question,(size_t)qn)) {
            answer=malloc((size_t)an+1);
            if (answer) { memcpy(answer,p+(size_t)qn,(size_t)an); answer[an]=0; }
            break;
        }
        p+=(size_t)qn+(size_t)an;
        if (*p=='\n') ++p;
    }
    free(data);
    return answer;
}

static void save_reference(const char *question,const char *answer)
{
    if (!reference_path[0] || !question || !answer || !*question || !*answer) return;
    size_t qn=strlen(question),an=strlen(answer);
    if (qn>65535 || an>262144) return;
    HANDLE file=CreateFileW(reference_path,FILE_APPEND_DATA,FILE_SHARE_READ,NULL,
                            OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return;
    char header[64];
    int hn=snprintf(header,sizeof(header),"%zu %zu\n",qn,an);
    DWORD written=0;
    if (hn>0 && WriteFile(file,header,(DWORD)hn,&written,NULL) && written==(DWORD)hn &&
        WriteFile(file,question,(DWORD)qn,&written,NULL) && written==(DWORD)qn &&
        WriteFile(file,answer,(DWORD)an,&written,NULL) && written==(DWORD)an)
        WriteFile(file,"\n",1,&written,NULL);
    FlushFileBuffers(file);
    CloseHandle(file);
}

static char *load_memory(void)
{
    if (!memory_path[0]) return NULL;
    HANDLE file=CreateFileW(memory_path,GENERIC_READ,FILE_SHARE_READ,NULL,
                            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file,&size) || size.QuadPart<=0 || size.QuadPart>65536) {
        CloseHandle(file); return NULL;
    }
    char *data=malloc((size_t)size.QuadPart+1);
    DWORD read=0;
    if (!data || !ReadFile(file,data,(DWORD)size.QuadPart,&read,NULL) ||
        read!=(DWORD)size.QuadPart) { free(data); data=NULL; }
    if (data) data[read]='\0';
    CloseHandle(file);
    return data;
}

static void save_memory(void)
{
    if (!memory_path[0] || !conversation) return;
    HANDLE file=CreateFileW(memory_path,GENERIC_WRITE,FILE_SHARE_READ,NULL,
                            CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return;
    size_t n=strlen(conversation);
    size_t offset=0;
    while (offset<n) {
        DWORD chunk=(DWORD)((n-offset)>65536?65536:(n-offset));
        DWORD written=0;
        if (!WriteFile(file,conversation+offset,chunk,&written,NULL) || !written) break;
        offset+=written;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
}

static void render_saved_conversation(void)
{
    if (!conversation) return;
    const char *p=conversation;
    const char *start_tag="<|im_start|>";
    const size_t tag_len=sizeof("<|im_start|>")-1;
    while ((p=strstr(p,start_tag))!=NULL) {
        const char *role=p+tag_len;
        const char *line=strchr(role,'\n');
        if (!line) break;
        const char *end=strstr(line+1,"<|im_end|>");
        if (!end) break;
        int user=(size_t)(line-role)==4 && !memcmp(role,"user",4);
        int assistant=(size_t)(line-role)==9 && !memcmp(role,"assistant",9);
        if (user || assistant) {
            append_wide(user?L"\r\nYou: ":L"\r\nApollo LLM 1.0: ");
            size_t n=(size_t)(end-(line+1));
            char *part=malloc(n+1);
            if (part) {
                memcpy(part,line+1,n); part[n]='\0';
                wchar_t *wide=utf8_to_wide(part);
                if (wide) { append_wide(wide); free(wide); }
                free(part);
            }
            append_wide(L"\r\n");
        }
        p=end+sizeof("<|im_end|>")-1;
    }
}

static char *wide_to_utf8(const wchar_t *wide)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (!s || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, s, n, NULL, NULL)) { free(s); return NULL; }
    return s;
}

static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (!w || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) { free(w); return NULL; }
    return w;
}

static size_t complete_utf8_prefix(const char *s,size_t length)
{
    size_t i=0;
    while (i<length) {
        unsigned char c=(unsigned char)s[i]; size_t n;
        if (c<0x80) n=1;
        else if (c>=0xc2 && c<=0xdf) n=2;
        else if (c>=0xe0 && c<=0xef) n=3;
        else if (c>=0xf0 && c<=0xf4) n=4;
        else { ++i; continue; }
        if (i+n>length) break;
        int valid=1;
        for (size_t j=1;j<n;++j)
            if (((unsigned char)s[i+j]&0xc0)!=0x80) valid=0;
        if (n==3 && ((c==0xe0&&(unsigned char)s[i+1]<0xa0) ||
                     (c==0xed&&(unsigned char)s[i+1]>=0xa0))) valid=0;
        if (n==4 && ((c==0xf0&&(unsigned char)s[i+1]<0x90) ||
                     (c==0xf4&&(unsigned char)s[i+1]>=0x90))) valid=0;
        if (!valid) { ++i; continue; }
        i+=n;
    }
    return i;
}

static void quote_arg(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    if (cap < 3) return;
    dst[o++] = '"';
    for (size_t i = 0; src[i] && o + 3 < cap; ++i) {
        if (src[i] == '"' || src[i] == '\\') dst[o++] = '\\';
        dst[o++] = src[i];
    }
    dst[o++] = '"'; dst[o] = 0;
}

static char *run_child_capture(Request *req,const char *command,DWORD *exit_code)
{
    if (exit_code) *exit_code=1;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    HANDLE reader=NULL,writer=NULL;
    if (!CreatePipe(&reader,&writer,&sa,0)) return NULL;
    SetHandleInformation(reader,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si={0}; PROCESS_INFORMATION pi={0};
    si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
    HANDLE nul=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                           &sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    si.hStdOutput=writer; si.hStdError=nul; si.hStdInput=nul;
    wchar_t *cmdw=utf8_to_wide(command);
    BOOL started=cmdw && CreateProcessW(NULL,cmdw,NULL,NULL,TRUE,CREATE_NO_WINDOW,
                                        NULL,NULL,&si,&pi);
    free(cmdw); CloseHandle(writer);
    if (nul!=INVALID_HANDLE_VALUE) CloseHandle(nul);
    char *response=NULL;
    if (!started) response=_strdup("[Could not start inference executable.]");
    else {
        size_t used=0,cap=8192; response=malloc(cap);
        char buf[2048]; DWORD got=0; int timed_out=0; ULONGLONG last_status=0;
        for (;;) {
            DWORD available=0;
            if (!PeekNamedPipe(reader,NULL,0,NULL,&available,NULL)) break;
            if (!available) {
                if (WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0) break;
                ULONGLONG now=GetTickCount64();
                if (now>=req->deadline_tick) {
                    timed_out=1; TerminateProcess(pi.hProcess,124);
                    WaitForSingleObject(pi.hProcess,INFINITE); break;
                }
                if (now-last_status>=250) {
                    LONG stage=InterlockedCompareExchange(&req->stage,0,0);
                    post_status(req,stage?L"Apollo reference is generating":"Apollo is generating",used);
                    last_status=now;
                }
                Sleep(40); continue;
            }
            DWORD amount=available<(DWORD)sizeof(buf)?available:(DWORD)sizeof(buf);
            if (!ReadFile(reader,buf,amount,&got,NULL) || !got) continue;
            if (!response) continue;
            if (used+(size_t)got+1>cap) {
                size_t next=cap;
                while (next<used+(size_t)got+1 && next<SIZE_MAX/2) next*=2;
                char *grown=next>=used+(size_t)got+1?realloc(response,next):NULL;
                if (!grown) { free(response); response=NULL; continue; }
                response=grown; cap=next;
            }
            memcpy(response+used,buf,got); used+=got; response[used]=0;
        }
        if (timed_out && response) {
            static const char timeout_note[]="\r\n[Response stopped at Apollo's five-minute limit; showing the partial answer.]\r\n";
            size_t a=strlen(response),b=sizeof(timeout_note)-1;
            char *grown=realloc(response,a+b+1);
            if (grown) { response=grown; memcpy(response+a,timeout_note,b+1); }
        }
        WaitForSingleObject(pi.hProcess,INFINITE);
        DWORD code=1; GetExitCodeProcess(pi.hProcess,&code);
        if (exit_code) *exit_code=code;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (!response) response=_strdup("");
    }
    CloseHandle(reader);
    if (response) {
        size_t n=strlen(response),valid=complete_utf8_prefix(response,n);
        response[valid]=0;
    }
    return response;
}

static float remove_confidence_marker(char *response,int *truncated)
{
    static const char marker[]="\036APOLLO_CONFIDENCE=";
    if (!response) return -1.0f;
    char *p=strstr(response,marker);
    if (!p) return -1.0f;
    char *value=p+sizeof(marker)-1;
    char *cut=strstr(value,",TRUNCATED=");
    if (truncated) *truncated=cut?atoi(cut+sizeof(",TRUNCATED=")-1):0;
    if (cut) *cut=0;
    float score=(float)atof(value);
    while (p>response && (p[-1]=='\r'||p[-1]=='\n')) --p;
    *p=0;
    return score;
}

static DWORD WINAPI inference_worker(void *param)
{
    Request *req = (Request *)param;
    char *model = wide_to_utf8(model_path);
#if !defined(APOLLO_COMPACT)
    char *language_model = wide_to_utf8(language_model_path);
    char *teacher_model=wide_to_utf8(L"model\\apollo-llm-1.0.gguf");
    char *qlanguage = NULL;
#endif
    char *question=latest_user_text(req->prompt_utf8);
    char *question_key=question?normalize_question(question):NULL;
    size_t n = strlen(req->prompt_utf8) + 4096;
    char *qmodel = malloc(n), *qprompt = malloc(n), *command = malloc(n * 2 + 512);
#if !defined(APOLLO_COMPACT)
    qlanguage = malloc(n); char *qteacher=malloc(n);
#endif
    if (!model || !question || !question_key || !qmodel || !qprompt || !command
#if !defined(APOLLO_COMPACT)
        || !language_model || !teacher_model || !qlanguage || !qteacher
#endif
    ) goto failed;
    quote_arg(qmodel, n, model);
    quote_arg(qprompt, n, req->prompt_utf8);
    char *response=find_reference(question_key);
    DWORD exit_code=0;
    int from_reference=response!=NULL;
    if (from_reference) {
        post_status(req,L"Using a saved Apollo reference",0);
        wchar_t *part=utf8_to_wide(response);
        if (part && !PostMessageW(req->window,WM_APPEND_TEXT,0,(LPARAM)part)) free(part);
    }
    if (!response) {
    post_status(req,L"Starting Apollo's local model",0);
#if defined(APOLLO_COMPACT)
    snprintf(command,n*2+512,INFER_EXE " %s %s %u %u --confidence",qmodel,qprompt,req->max_tokens,req->min_tokens);
#else
    quote_arg(qlanguage, n, language_model);
    quote_arg(qteacher,n,teacher_model);
    snprintf(command,n*2+512,INFER_EXE " %s %s %u %u %s --confidence",qmodel,qprompt,req->max_tokens,req->min_tokens,qlanguage);
#endif
    response=run_child_capture(req,command,&exit_code);
    float confidence=remove_confidence_marker(response,NULL);
#if !defined(APOLLO_COMPACT)
    if (exit_code==0 && confidence>=0.0f && confidence<0.25f) {
        InterlockedExchange(&req->stage,1);
        post_status(req,L"Consulting Apollo's 1.5B reference",0);
        wchar_t *notice=utf8_to_wide("\r\n[Low confidence; checking Apollo's 1.5B reference model...]\r\n");
        if (notice && !PostMessageW(req->window,WM_APPEND_TEXT,0,(LPARAM)notice)) free(notice);
        snprintf(command,n*2+512,"apollo_teacher_infer.exe %s %s %u %u --confidence",
                 qteacher,qprompt,req->max_tokens,req->min_tokens);
        DWORD teacher_exit=1;
        char *teacher=run_child_capture(req,command,&teacher_exit);
        int teacher_truncated=0;
        (void)remove_confidence_marker(teacher,&teacher_truncated);
        if (teacher_exit==0 && teacher && *teacher) {
            free(response); response=teacher;
            if (!teacher_truncated) save_reference(question_key,response);
        } else free(teacher);
    }
#endif
    }
    if (response && !from_reference) {
        wchar_t *part=utf8_to_wide(response);
        if (part && !PostMessageW(req->window,WM_APPEND_TEXT,0,(LPARAM)part)) free(part);
    }
    if (exit_code!=0 && exit_code!=124 && response) {
        size_t a=strlen(response),b=strlen("\r\n[Inference failed.]\r\n");
        char *grown=realloc(response,a+b+1);
        if (grown) { response=grown; memcpy(response+a,"\r\n[Inference failed.]\r\n",b+1); }
    }
    free(model); free(qmodel); free(qprompt); free(command);
#if !defined(APOLLO_COMPACT)
    free(language_model); free(teacher_model); free(qlanguage); free(qteacher);
#endif
    free(question);
    free(question_key);
    {
        Response *result = calloc(1, sizeof(*result));
        if (result) {
            result->context = req->prompt_utf8;
            result->text = response ? response : _strdup("\r\n[Could not allocate inference output.]\r\n");
            PostMessageW(req->window, WM_REQUEST_DONE, 0, (LPARAM)result);
        } else { free(response); free(req->prompt_utf8); PostMessageW(req->window, WM_REQUEST_DONE, 0, 0); }
        free(req);
    }
    return 0;
failed:
    free(model); free(qmodel); free(qprompt); free(command);
#if !defined(APOLLO_COMPACT)
    free(language_model); free(teacher_model); free(qlanguage); free(qteacher);
#endif
    free(question);
    free(question_key);
    {
        Response *result = calloc(1, sizeof(*result));
        if (result) {
            result->context = req->prompt_utf8;
            result->text = _strdup("\r\n[Could not launch inference worker.]\r\n");
            PostMessageW(req->window, WM_REQUEST_DONE, 0, (LPARAM)result);
        } else { free(req->prompt_utf8); PostMessageW(req->window, WM_REQUEST_DONE, 0, 0); }
        free(req);
    }
    return 1;
}

static void start_request(HWND window)
{
    int chars = GetWindowTextLengthW(prompt_edit);
    if (chars <= 0) { MessageBoxW(window, L"Type a message first.", L"Empty message", MB_OK | MB_ICONINFORMATION); return; }
    wchar_t *wide = malloc(((size_t)chars + 1) * sizeof(wchar_t));
    if (!wide) return;
    GetWindowTextW(prompt_edit, wide, chars + 1);
    char *message = wide_to_utf8(wide); free(wide);
    if (!message) return;
    size_t oldlen = conversation ? strlen(conversation) : 0;
    size_t newlen = oldlen + strlen(message) + 100;
    char *utf8 = malloc(newlen);
    if (!utf8) { free(message); return; }
    if (oldlen) memcpy(utf8, conversation, oldlen); else utf8[0] = 0;
    snprintf(utf8 + oldlen, newlen - oldlen,
             "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n", message);
    wchar_t *echo = utf8_to_wide(message);
    append_wide(L"\r\nYou: "); if (echo) { append_wide(echo); free(echo); }
#if defined(APOLLO_COMPACT)
    append_wide(L"\r\nApollo LLM 1.0 (Coder 100%): ");
#else
    append_wide(message_needs_code_model(message)
        ? L"\r\nApollo LLM 1.0 (Coder 100%, Language 0%): "
        : L"\r\nApollo LLM 1.0 (Language 100%, Coder 0%): ");
#endif
    append_wide(L"\r\n");
    SetWindowTextW(prompt_edit, L"");
    EnableWindow(send_button, FALSE);
    SetWindowTextW(status_label,L"Starting Apollo's local model...");
    Request *req = malloc(sizeof(*req));
    if (!req) { free(utf8); EnableWindow(send_button, TRUE); return; }
    req->window = window; req->prompt_utf8 = utf8;
    req->deadline_tick=GetTickCount64()+300000ULL;
    req->stage=0;
    req->min_tokens=message_needs_code_model(message) ? 0 : min_reply_tokens(message);
    req->max_tokens=message_needs_code_model(message) || message_needs_decomposition(message)
        ? 512 : (req->min_tokens ? 256 : 128);
    free(message);
    char *pending=_strdup(utf8);
    if (pending) { free(conversation); conversation=pending; save_memory(); }
    HANDLE thread = CreateThread(NULL, 0, inference_worker, req, 0, NULL);
    if (!thread) { free(utf8); free(req); EnableWindow(send_button, TRUE); append_wide(L"Could not start inference worker.\r\n"); }
    else CloseHandle(thread);
}

static LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        search_edit = CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
                                      16,12,700,28,window,(HMENU)ID_SEARCH,NULL,NULL);
        CreateWindowW(L"STATIC",L"Find in chat",WS_CHILD|WS_VISIBLE,16,47,90,22,window,NULL,NULL,NULL);
        MoveWindow(search_edit,108,44,560,28,FALSE);
        send_button = CreateWindowW(L"BUTTON", L"Find", WS_CHILD | WS_VISIBLE,
                                    680, 42, 100, 32, window, (HMENU)ID_FIND, NULL, NULL);
        prompt_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                      16, 80, 650, 74, window, (HMENU)ID_PROMPT, NULL, NULL);
        send_button = CreateWindowW(L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                    680, 80, 100, 32, window, (HMENU)ID_SEND, NULL, NULL);
        status_label=CreateWindowW(L"STATIC",L"Ready",WS_CHILD|WS_VISIBLE,16,158,764,22,window,(HMENU)ID_STATUS,NULL,NULL);
        output_box = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                      MODEL_LABEL L"\r\nLocal C inference. Complex requests can take up to five minutes; progress appears above.\r\n",
                      WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                      16, 184, 764, 368, window, (HMENU)ID_LOG, NULL, NULL);
        ui_font=CreateFontW(-18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
                            DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        apply_ui_font(search_edit); apply_ui_font(prompt_edit); apply_ui_font(output_box);
        apply_ui_font(status_label);
        HWND find_button=GetDlgItem(window,ID_FIND); apply_ui_font(find_button);
        apply_ui_font(send_button);
        if (conversation_restored) render_saved_conversation();
        return 0;
    case WM_SIZE:
        if (output_box) {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(search_edit,108,12,w-220,28,TRUE);
            MoveWindow(GetDlgItem(window,ID_FIND),w-100,10,84,32,TRUE);
            MoveWindow(prompt_edit,16,48,w-148,82,TRUE);
            MoveWindow(send_button,w-116,48,100,36,TRUE);
            MoveWindow(status_label,16,136,w-32,24,TRUE);
            MoveWindow(output_box,16,166,w-32,h-182,TRUE);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEND) { start_request(window); return 0; }
        if (LOWORD(wp) == ID_FIND) { find_in_chat(window); return 0; }
        if (LOWORD(wp)==ID_SEARCH && HIWORD(wp)==EN_SETFOCUS) return 0;
        break;
    case WM_KEYDOWN:
        if (wp=='F' && (GetKeyState(VK_CONTROL)&0x8000)) { SetFocus(search_edit); return 0; }
        break;
    case WM_INFERENCE_PROGRESS: {
        wchar_t *status=(wchar_t*)lp;
        if (status) { SetWindowTextW(status_label,status); free(status); }
        return 0;
    }
    case WM_APPEND_TEXT: {
        wchar_t *wide = (wchar_t *)lp;
        if (wide) { append_wide(wide); free(wide); }
        return 0;
    }
    case WM_REQUEST_DONE: {
        Response *result = (Response *)lp;
        if (result) {
            const char *message=result->text ? result->text : "";
            const char *error=strstr(message,"[Inference failed.");
            if (!error) error=strstr(message,"Could not start " INFER_EXE);
            if (!error) error=strstr(message,"Could not launch inference worker.");
            if (error) {
                wchar_t *notice=utf8_to_wide(error);
                if (notice) { append_wide(notice); free(notice); }
            }
            size_t a = strlen(result->context ? result->context : "");
            size_t b = strlen(result->text ? result->text : "");
            static const char turn_end[] = "<|im_end|>\n";
            char *next = malloc(a + b + sizeof(turn_end));
            if (next) {
                memcpy(next, result->context, a);
                memcpy(next + a, result->text, b);
                memcpy(next + a + b, turn_end, sizeof(turn_end));
                free(conversation); conversation = next;
                size_t len = strlen(conversation);
                if (len > 10000) {
                    const char *cut = conversation + len - 7000;
                    const char *boundary = strstr(cut, "<|im_start|>user\n");
                    if (boundary) {
                        const char *system = system_message;
                        size_t prefix = strlen(system);
                        size_t tail = strlen(boundary);
                        char *trimmed = malloc(prefix + tail + 1);
                        if (trimmed) { memcpy(trimmed, system, prefix); memcpy(trimmed + prefix, boundary, tail + 1); free(conversation); conversation = trimmed; }
                    }
                }
                save_memory();
            }
            free(result->context); free(result->text); free(result);
        }
        EnableWindow(send_button, TRUE);
        SetWindowTextW(status_label,L"Ready");
        return 0;
    }
    case WM_DESTROY: save_memory(); PostQuitMessage(0); return 0;
    case WM_DESTROY + 100: return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR cmdline, int show)
{
    (void)previous; (void)cmdline;
    init_memory_path();
    conversation=load_memory();
    if (conversation) {
        const char *old_system_end=strstr(conversation,"<|im_end|>\n");
        if (old_system_end && !strncmp(conversation,"<|im_start|>system\n",19)) {
            const char *history=old_system_end+sizeof("<|im_end|>\n")-1;
            size_t prefix=strlen(system_message), tail=strlen(history);
            char *updated=malloc(prefix+tail+1);
            if (updated) {
                memcpy(updated,system_message,prefix);
                memcpy(updated+prefix,history,tail+1);
                free(conversation); conversation=updated;
                save_memory();
            }
        }
        conversation_restored=1;
    } else conversation=_strdup(system_message);
    WNDCLASSW wc = {0}; wc.lpfnWndProc = window_proc; wc.hInstance = instance;
    wc.lpszClassName = L"ApolloChatWindow"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) return 1;
    HWND window = CreateWindowW(wc.lpszClassName, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 820, 620, NULL, NULL, instance, NULL);
    if (!window) return 1;
    ShowWindow(window, show);
#if defined(APOLLO_UI_DESIGN_TEST)
    ShowWindow(window,SW_HIDE);
    SetWindowTextW(prompt_edit,
        L"Apollo, design a better user interface for yourself. Break the interface proposal into a few small parts and decide which changes give the most usability benefit for the least processing and implementation work. The current app is a native Win32 chat window. Propose a practical prioritized design that improves readability, conversation navigation, composing and sending, response status, and model/reference visibility. Describe the layout, controls, and keyboard behavior. Keep it realistic to implement in plain C Win32, concise enough that we can implement it, and do not write source code yet.");
    PostMessageW(window,WM_COMMAND,ID_SEND,0);
#endif
    MSG msg; while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}
