# Lestrix shell integration for bash: marks prompts and command output (OSC 133) and reports the directory (OSC 7).
# Add to ~/.bashrc:  [ -n "$LESTRIX" ] || [ "$TERM_PROGRAM" = lestrix ] && source /path/to/bash.sh   (or just source it)
case $- in *i*) ;; *) return 0 ;; esac
[ -n "${__lx_loaded:-}" ] && return 0
__lx_loaded=1

__lx_osc7() {
    local p=$PWD out= c i
    for ((i = 0; i < ${#p}; i++)); do
        c=${p:i:1}
        case $c in [a-zA-Z0-9/._~-]) out+=$c ;; *) printf -v c '%%%02X' "'$c"; out+=$c ;; esac
    done
    printf '\e]7;file://%s%s\a' "${HOSTNAME:-}" "$out"
}
__lx_precmd() {
    local ec=$?
    printf '\e]133;D;%s\a' "$ec"
    __lx_osc7
    return $ec
}
if [[ $(declare -p PROMPT_COMMAND 2>/dev/null) == "declare -a"* ]]; then
    PROMPT_COMMAND=(__lx_precmd "${PROMPT_COMMAND[@]}")
else
    PROMPT_COMMAND="__lx_precmd${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
fi
PS1='\[\e]133;A\a\]'"$PS1"'\[\e]133;B\a\]'
PS0='\e]133;C\a'"${PS0:-}"
