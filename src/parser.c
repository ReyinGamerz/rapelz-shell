#define _POSIX_C_SOURCE 200809L
#include "rapelz.h"

#include <ctype.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int putc_cap(char *o, size_t *n, size_t cap, char c) {
    if (*n + 1 >= cap) return -1;
    o[(*n)++] = c;
    o[*n] = '\0';
    return 0;
}

/* Simpan nilai argumen dan pola glob secara terpisah: quote tidak boleh
 * mengubah hasil literal, tetapi harus melindungi metakarakter pada pola. */
static int word_putc(char *out, size_t *out_len, size_t out_cap,
                     char *pattern, size_t *pattern_len, size_t pattern_cap,
                     char c, int quoted) {
    if (putc_cap(out, out_len, out_cap, c) != 0) return -1;
    if (quoted && strchr("*?[]\\", c) &&
        putc_cap(pattern, pattern_len, pattern_cap, '\\') != 0) return -1;
    return putc_cap(pattern, pattern_len, pattern_cap, c);
}

static int word_puts(char *out, size_t *out_len, size_t out_cap,
                     char *pattern, size_t *pattern_len, size_t pattern_cap,
                     const char *text, int quoted, int *globable) {
    if (!text) return 0;
    while (*text) {
        if (!quoted && strchr("*?[", *text)) *globable = 1;
        if (word_putc(out, out_len, out_cap, pattern, pattern_len,
                      pattern_cap, *text++, quoted) != 0) return -1;
    }
    return 0;
}

static int word_env_put(char *out, size_t *out_len, size_t out_cap,
                        char *pattern, size_t *pattern_len, size_t pattern_cap,
                        const char *name, size_t name_len, int quoted,
                        int *globable) {
    char key[256];
    if (!name_len || name_len >= sizeof(key)) return -1;
    memcpy(key, name, name_len);
    key[name_len] = '\0';
    const char *value = getenv(key);
    return word_puts(out, out_len, out_cap, pattern, pattern_len,
                     pattern_cap, value ? value : "", quoted, globable);
}
static int find_sub_end(const char *s,size_t start,size_t *end) {
    int depth=1,sq=0,dq=0;
    for (size_t i=start;s[i];i++) {
        char c=s[i];
        if (sq) { if (c=='\\' && s[i+1]) { i++; continue; } if(c=='\'') sq=0; continue; }
        if (dq) { if (c=='\\' && s[i+1]) { i++; continue; } if(c=='"') dq=0; continue; }
        if(c=='\'') sq=1;
        else if(c=='"') dq=1;
        else if(c=='\\' && s[i+1]) i++;
        else if(c=='$' && s[i+1]=='(') { depth++; i++; }
        else if(c==')') { if(--depth==0){*end=i;return 0;} }
    }
    return -1;
}

static int expand_word(const char *raw, char *out, size_t cap,
                        char *pattern, size_t pattern_cap, int *globable) {
    size_t n = 0, pn = 0;
    int sq = 0, dq = 0, start = 1;

    *globable = 0;
    out[0] = '\0';
    pattern[0] = '\0';

    for (size_t i = 0; raw[i]; i++) {
        char c = raw[i];

        if (sq) {
            if (c == '\'') sq = 0;
            else if (word_putc(out, &n, cap, pattern, &pn, pattern_cap,
                               c, 1) != 0) return -1;
            start = 0;
            continue;
        }

        if (dq) {
            if (c == '"') {
                dq = 0;
                continue;
            }
            if (c == '\\' && raw[i + 1] &&
                strchr("\"\\$", raw[i + 1])) {
                if (word_putc(out, &n, cap, pattern, &pn, pattern_cap,
                              raw[++i], 1) != 0) return -1;
                start = 0;
                continue;
            }
            if (c == '$') {
                if (raw[i + 1] == '(') {
                    size_t sub_end;
                    if (find_sub_end(raw, i + 2, &sub_end) != 0) {
                        fprintf(stderr, "rapelz: sintaksis: command substitution belum ditutup\n");
                        return -1;
                    }
                    size_t sub_len = sub_end - i - 2;
                    char *inner = malloc(sub_len + 1);
                    if (!inner) return -1;
                    memcpy(inner, raw + i + 2, sub_len);
                    inner[sub_len] = '\0';
                    char *captured = NULL;
                    size_t captured_len = 0;
                    int rc = rapelz_capture_command(inner, &captured, &captured_len);
                    free(inner);
                    if (rc != 0) {
                        free(captured);
                        return -1;
                    }
                    while (captured_len && captured[captured_len - 1] == '\n')
                        captured[--captured_len] = '\0';
                    rc = word_puts(out, &n, cap, pattern, &pn, pattern_cap,
                                   captured, 1, globable);
                    free(captured);
                    if (rc != 0) return -1;
                    i = sub_end;
                    start = 0;
                    continue;
                }
                if (raw[i + 1] == '?') {
                    char value[32];
                    snprintf(value, sizeof(value), "%d", rapelz_last_status());
                    if (word_puts(out, &n, cap, pattern, &pn, pattern_cap,
                                  value, 1, globable) != 0) return -1;
                    i++;
                    start = 0;
                    continue;
                }
                if (raw[i + 1] == '$') {
                    char value[32];
                    snprintf(value, sizeof(value), "%ld", (long)getpid());
                    if (word_puts(out, &n, cap, pattern, &pn, pattern_cap,
                                  value, 1, globable) != 0) return -1;
                    i++;
                    start = 0;
                    continue;
                }
                if (raw[i + 1] == '{') {
                    size_t j = i + 2;
                    while (raw[j] && raw[j] != '}') j++;
                    if (raw[j] != '}') {
                        fprintf(stderr, "rapelz: sintaksis: parameter expansion belum ditutup\n");
                        return -1;
                    }
                    if (word_env_put(out, &n, cap, pattern, &pn, pattern_cap,
                                     raw + i + 2, j - i - 2, 1, globable) != 0) return -1;
                    i = j;
                    start = 0;
                    continue;
                }
                size_t j = i + 1;
                while (isalnum((unsigned char)raw[j]) || raw[j] == '_') j++;
                if (j > i + 1) {
                    if (word_env_put(out, &n, cap, pattern, &pn, pattern_cap,
                                     raw + i + 1, j - i - 1, 1, globable) != 0) return -1;
                    i = j - 1;
                    start = 0;
                    continue;
                }
            }
            if (word_putc(out, &n, cap, pattern, &pn, pattern_cap, c, 1) != 0)
                return -1;
            start = 0;
            continue;
        }

        if (c == '\'') {
            sq = 1;
            continue;
        }
        if (c == '"') {
            dq = 1;
            continue;
        }
        if (c == '\\') {
            if (!raw[i + 1]) {
                fprintf(stderr, "rapelz: sintaksis: backslash tanpa karakter lanjutan\n");
                return -1;
            }
            if (word_putc(out, &n, cap, pattern, &pn, pattern_cap,
                          raw[++i], 1) != 0) return -1;
            start = 0;
            continue;
        }

        if (c == '$') {
            if (raw[i + 1] == '(') {
                size_t sub_end;
                if (find_sub_end(raw, i + 2, &sub_end) != 0) {
                    fprintf(stderr, "rapelz: sintaksis: command substitution belum ditutup\n");
                    return -1;
                }
                size_t sub_len = sub_end - i - 2;
                char *inner = malloc(sub_len + 1);
                if (!inner) return -1;
                memcpy(inner, raw + i + 2, sub_len);
                inner[sub_len] = '\0';
                char *captured = NULL;
                size_t captured_len = 0;
                int rc = rapelz_capture_command(inner, &captured, &captured_len);
                free(inner);
                if (rc != 0) {
                    free(captured);
                    return -1;
                }
                while (captured_len && captured[captured_len - 1] == '\n')
                    captured[--captured_len] = '\0';
                rc = word_puts(out, &n, cap, pattern, &pn, pattern_cap,
                               captured, 0, globable);
                free(captured);
                if (rc != 0) return -1;
                i = sub_end;
                start = 0;
                continue;
            }
            if (raw[i + 1] == '?') {
                char value[32];
                snprintf(value, sizeof(value), "%d", rapelz_last_status());
                if (word_puts(out, &n, cap, pattern, &pn, pattern_cap,
                              value, 0, globable) != 0) return -1;
                i++;
                start = 0;
                continue;
            }
            if (raw[i + 1] == '$') {
                char value[32];
                snprintf(value, sizeof(value), "%ld", (long)getpid());
                if (word_puts(out, &n, cap, pattern, &pn, pattern_cap,
                              value, 0, globable) != 0) return -1;
                i++;
                start = 0;
                continue;
            }
            if (raw[i + 1] == '{') {
                size_t j = i + 2;
                while (raw[j] && raw[j] != '}') j++;
                if (raw[j] != '}') {
                    fprintf(stderr, "rapelz: sintaksis: parameter expansion belum ditutup\n");
                    return -1;
                }
                if (word_env_put(out, &n, cap, pattern, &pn, pattern_cap,
                                 raw + i + 2, j - i - 2, 0, globable) != 0) return -1;
                i = j;
                start = 0;
                continue;
            }
            size_t j = i + 1;
            while (isalnum((unsigned char)raw[j]) || raw[j] == '_') j++;
            if (j > i + 1) {
                if (word_env_put(out, &n, cap, pattern, &pn, pattern_cap,
                                 raw + i + 1, j - i - 1, 0, globable) != 0) return -1;
                i = j - 1;
                start = 0;
                continue;
            }
        }

        if (start && c == '~' && (raw[i + 1] == '\0' || raw[i + 1] == '/')) {
            const char *home = getenv("HOME");
            if (home) {
                if (word_puts(out, &n, cap, pattern, &pn, pattern_cap,
                              home, 0, globable) != 0) return -1;
            } else if (word_putc(out, &n, cap, pattern, &pn, pattern_cap,
                                 '~', 0) != 0) return -1;
            start = 0;
            continue;
        }

        if (c == '*' || c == '?' || c == '[') *globable = 1;
        if (word_putc(out, &n, cap, pattern, &pn, pattern_cap, c, 0) != 0)
            return -1;
        start = 0;
    }

    if (sq || dq) {
        fprintf(stderr, "rapelz: sintaksis: quote belum ditutup\n");
        return -1;
    }
    return 0;
}

static int append_raw(rapelz_command_t *cmd,const char *raw) {
    if(cmd->argc>=RAPELZ_MAX_ARGS-1){fprintf(stderr,"rapelz: terlalu banyak argumen\n");return -1;}
    cmd->argv[cmd->argc]=strdup(raw); if(!cmd->argv[cmd->argc])return -1;
    cmd->argc++;cmd->argv[cmd->argc]=NULL;return 0;
}

static int parse_word(const char *s,size_t *idx,char *out,size_t cap) {
    size_t n=0;int sq=0,dq=0,paren=0;
    while(s[*idx]) {
        char c=s[*idx];
        if(!sq&&!dq&&!paren &&
           (isspace((unsigned char)c)||c=='|'||c=='&'||c==';'||c=='<'||c=='>')) break;

        if(!sq&&c=='\\') {
            if(n+2>=cap)return -1;
            out[n++]=c;(*idx)++;
            if(s[*idx])out[n++]=s[(*idx)++];
            continue;
        }
        if(!sq&&c=='"') dq=!dq;
        else if(!dq&&c=='\'') sq=!sq;
        else if(!sq&&!dq&&c=='$'&&s[*idx+1]=='(') paren++;
        else if(!sq&&!dq&&paren&&c=='('&&(*idx==0||s[*idx-1]!='$')) paren++;
        else if(!sq&&!dq&&paren&&c==')') paren--;

        if(n+1>=cap)return -1;
        out[n++]=s[(*idx)++];
    }
    if(sq||dq||paren)return -2;
    if(!n)return -1;
    out[n]='\0';return 0;
}

static int redirection_token_boundary(char c) {
    return c == '\0' || isspace((unsigned char)c) ||
           c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

static int parse_redirection(const char *s,size_t *idx,rapelz_command_t *cmd) {
    size_t save=*idx;int fd=-1;
    if(isdigit((unsigned char)s[*idx])) {
        size_t j=*idx;while(isdigit((unsigned char)s[*idx]))(*idx)++;
        if(s[*idx]!='<'&&s[*idx]!='>'){*idx=save;return 0;}
        char num[16];size_t n=*idx-j;if(n>=sizeof(num))return -1;
        memcpy(num,s+j,n);num[n]='\0';
        char *end = NULL;
        long parsed_fd = strtol(num, &end, 10);
        if (!end || *end != '\0' || parsed_fd < 0 || parsed_fd >= INT_MAX) {
            fprintf(stderr, "rapelz: descriptor redirection tidak valid\n");
            return -1;
        }
        fd = (int)parsed_fd;
    }

    if(s[*idx]=='&'&&s[*idx+1]=='>') {
        if(cmd->redir_count>=RAPELZ_MAX_REDIRS)return -1;
        (*idx)+=2;rapelz_redirection_t r={0};r.fd=STDOUT_FILENO;r.type=RAPELZ_REDIR_OUT;
        if(s[*idx]=='>'){r.type=RAPELZ_REDIR_APPEND;(*idx)++;}
        while(isspace((unsigned char)s[*idx]))(*idx)++;
        char raw[RAPELZ_INPUT_SIZE];
        if(parse_word(s,idx,raw,sizeof(raw))!=0)return -1;
        r.target=strdup(raw);r.dup_fd=-2;if(!r.target)return -1;
        cmd->redirs[cmd->redir_count++]=r;return 1;
    }

    if(s[*idx]!='<'&&s[*idx]!='>'){*idx=save;return 0;}
    char op=s[*idx];(*idx)++;
    rapelz_redir_type_t type=(op=='<')?RAPELZ_REDIR_IN:RAPELZ_REDIR_OUT;
    if(op=='>'&&s[*idx]=='>'){type=RAPELZ_REDIR_APPEND;(*idx)++;}

    if(s[*idx]=='&') {
        (*idx)++;
        int dup=-2;
        if(s[*idx]=='-'){dup=-1;(*idx)++;}
        else if(isdigit((unsigned char)s[*idx])) {
            dup=0;
            while(isdigit((unsigned char)s[*idx])) {
                if(dup>1000000)return -1;
                dup=dup*10+(s[*idx]-'0');
                (*idx)++;
            }
        }
        if(dup==-2 || !redirection_token_boundary(s[*idx])) {
            fprintf(stderr,"rapelz: sintaksis: descriptor duplikasi redirection tidak valid\n");
            return -1;
        }
        if(cmd->redir_count>=RAPELZ_MAX_REDIRS)return -1;
        rapelz_redirection_t r={0};
        r.fd=fd>=0?fd:(op=='<'?STDIN_FILENO:STDOUT_FILENO);
        r.type=dup<0?RAPELZ_REDIR_CLOSE:(op=='<'?RAPELZ_REDIR_DUP_IN:RAPELZ_REDIR_DUP_OUT);
        r.dup_fd=dup;cmd->redirs[cmd->redir_count++]=r;return 1;
    }

    while(isspace((unsigned char)s[*idx]))(*idx)++;
    char raw[RAPELZ_INPUT_SIZE];
    if(parse_word(s,idx,raw,sizeof(raw))!=0)return -1;
    if(cmd->redir_count>=RAPELZ_MAX_REDIRS)return -1;
    rapelz_redirection_t r={0};
    r.fd=fd>=0?fd:(op=='<'?STDIN_FILENO:STDOUT_FILENO);
    r.type=type;r.target=strdup(raw);r.dup_fd=-1;
    if(!r.target)return -1;
    cmd->redirs[cmd->redir_count++]=r;return 1;
}

static int start_pipeline(rapelz_program_t *p) {
    if(p->pipeline_count>=RAPELZ_MAX_PIPELINES){fprintf(stderr,"rapelz: terlalu banyak pipeline\n");return -1;}
    rapelz_pipeline_t *pl=&p->pipelines[p->pipeline_count++];
    memset(pl,0,sizeof(*pl));pl->command_count=1;return 0;
}

int rapelz_parse_line(const char *line,rapelz_program_t *program) {
    if(!line||!program)return -1;
    memset(program,0,sizeof(*program));

    /*
     * Baris kosong dan komentar penuh adalah no-op yang valid, seperti pada
     * shell POSIX. Jangan membuat pipeline kosong yang kemudian dianggap
     * sebagai kesalahan sintaks.
     */
    const char *first = line;
    while (isspace((unsigned char)*first)) first++;
    if (*first == '\0' || *first == '#') return 0;

    if(start_pipeline(program)!=0)return -1;
    size_t i=0;

    while(1) {
        while(isspace((unsigned char)line[i]))i++;
        if(!line[i])break;
        rapelz_pipeline_t *pl=&program->pipelines[program->pipeline_count-1];
        rapelz_command_t *cmd=&pl->commands[pl->command_count-1];

        if(line[i]=='#')break;

        int rr=parse_redirection(line,&i,cmd);
        if(rr<0)goto syntax;
        if(rr>0)continue;

        if(line[i]==';'||(line[i]=='&'&&line[i+1]=='&')||(line[i]=='|'&&line[i+1]=='|')) {
            if(!cmd->argc)goto syntax;
            rapelz_connector_t c=line[i]==';'?RAPELZ_CONN_SEMI:(line[i]=='&'?RAPELZ_CONN_AND:RAPELZ_CONN_OR);
            i += line[i]==';'?1:2;
            program->connectors[program->pipeline_count-1]=c;
            if(start_pipeline(program)!=0)goto syntax;
            continue;
        }
        if(line[i]=='|'&&line[i+1]=='&') {
            if(!cmd->argc||pl->command_count>=RAPELZ_MAX_PIPE_COMMANDS)goto syntax;
            cmd->pipe_stderr=1;i+=2;memset(&pl->commands[pl->command_count],0,sizeof(pl->commands[0]));pl->command_count++;continue;
        }
        if(line[i]=='|') {
            if(!cmd->argc||pl->command_count>=RAPELZ_MAX_PIPE_COMMANDS)goto syntax;
            i++;memset(&pl->commands[pl->command_count],0,sizeof(pl->commands[0]));pl->command_count++;continue;
        }
        if(line[i]=='&') {
            if(!cmd->argc)goto syntax;
            pl->background=1;i++;
            while(isspace((unsigned char)line[i]))i++;
            if(!line[i])break;
            program->connectors[program->pipeline_count-1]=RAPELZ_CONN_SEMI;
            if(start_pipeline(program)!=0)goto syntax;
            continue;
        }

        char raw[RAPELZ_INPUT_SIZE];
        int rc=parse_word(line,&i,raw,sizeof(raw));
        if(rc==-2){fprintf(stderr,"rapelz: sintaksis: quote belum ditutup atau command substitution belum ditutup\n");goto fail;}
        if(rc!=0)goto syntax;
        if(append_raw(cmd,raw)!=0)goto fail;
    }

    if(program->pipeline_count>1) {
        rapelz_pipeline_t *last=&program->pipelines[program->pipeline_count-1];
        if(last->command_count==1&&last->commands[0].argc==0) {
            rapelz_connector_t previous =
                program->connectors[program->pipeline_count-2];
            /*
             * Semicolon boleh mengakhiri baris, termasuk sebelum komentar.
             * AND/OR yang tidak diikuti command tetap merupakan sintaks salah.
             */
            if(previous!=RAPELZ_CONN_NONE && previous!=RAPELZ_CONN_SEMI)
                goto syntax;
            program->pipeline_count--;
        }
    }

    for(int p=0;p<program->pipeline_count;p++) {
        rapelz_pipeline_t *pl=&program->pipelines[p];
        for(int c=0;c<pl->command_count;c++)if(pl->commands[c].argc==0)goto syntax;
        if(p+1<program->pipeline_count&&program->connectors[p]==RAPELZ_CONN_NONE)goto syntax;
    }
    return 0;

syntax:
    fprintf(stderr,"rapelz: sintaksis: operator atau command tidak valid\n");
fail:
    rapelz_free_program(program);return -1;
}

int rapelz_expand_command(rapelz_command_t *cmd) {
    if(!cmd)return -1;
    char *newv[RAPELZ_MAX_ARGS]={0};int argc=0;

    for(int i=0;i<cmd->argc;i++) {
        char out[RAPELZ_INPUT_SIZE];
        char pattern[RAPELZ_INPUT_SIZE * 2];
        int has_glob=0;
        if(expand_word(cmd->argv[i], out, sizeof(out), pattern,
                       sizeof(pattern), &has_glob)!=0) goto fail;
        if(has_glob) {
            glob_t g={0};int rc=glob(pattern,0,NULL,&g);
            if(rc==0&&g.gl_pathc) {
                for(size_t j=0;j<g.gl_pathc;j++) {
                    if(argc>=RAPELZ_MAX_ARGS-1){globfree(&g);goto fail;}
                    newv[argc]=strdup(g.gl_pathv[j]);if(!newv[argc++]){globfree(&g);goto fail;}
                }
            } else {
                newv[argc]=strdup(out);if(!newv[argc++]){globfree(&g);goto fail;}
            }
            globfree(&g);
        } else {
            newv[argc]=strdup(out);if(!newv[argc++])goto fail;
        }
    }
    newv[argc]=NULL;
    for(int i=0;i<cmd->argc;i++)free(cmd->argv[i]);
    for(int i=0;i<argc;i++)cmd->argv[i]=newv[i];
    for(int i=argc;i<RAPELZ_MAX_ARGS;i++)cmd->argv[i]=NULL;
    cmd->argc=argc;

    for(int i=0;i<cmd->redir_count;i++) {
        if(!cmd->redirs[i].target)continue;
        char out[RAPELZ_INPUT_SIZE];
        char pattern[RAPELZ_INPUT_SIZE * 2];
        int dummy=0;
        if(expand_word(cmd->redirs[i].target, out, sizeof(out), pattern,
                       sizeof(pattern), &dummy)!=0) return -1;
        free(cmd->redirs[i].target);cmd->redirs[i].target=strdup(out);
        if(!cmd->redirs[i].target)return -1;
    }
    return 0;

fail:
    for(int i=0;i<argc;i++)free(newv[i]);
    fprintf(stderr,"rapelz: gagal melakukan expansion\n");return -1;
}

void rapelz_free_program(rapelz_program_t *program) {
    if(!program)return;
    for(int p=0;p<program->pipeline_count;p++) {
        rapelz_pipeline_t *pl=&program->pipelines[p];
        for(int c=0;c<pl->command_count;c++) {
            rapelz_command_t *cmd=&pl->commands[c];
            for(int a=0;a<cmd->argc;a++)free(cmd->argv[a]);
            for(int r=0;r<cmd->redir_count;r++)free(cmd->redirs[r].target);
            cmd->argc=0;cmd->redir_count=0;
        }
    }
}
