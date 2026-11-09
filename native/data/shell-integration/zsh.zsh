# Lestrix shell integration for zsh: marks prompts and command output (OSC 133) and reports the directory (OSC 7).
# Add to ~/.zshrc:  source /path/to/zsh.zsh
[[ -o interactive ]] || return 0
(( ${+__lx_loaded} )) && return 0
typeset -g __lx_loaded=1 __lx_ran=0

__lx_osc7() {
    local LC_ALL=C i c out=
    for ((i = 1; i <= ${#PWD}; i++)); do
        c=${PWD[i]}
        case $c in [a-zA-Z0-9/._~-]) out+=$c ;; *) out+=$(printf '%%%02X' "'$c") ;; esac
    done
    printf '\e]7;file://%s%s\a' "${HOST}" "$out"
}
__lx_preexec() { __lx_ran=1; printf '\e]133;C\a' }
__lx_precmd() {
    local ec=$?   # must stay the first statement: any other hook that runs first would clobber $?
    (( __lx_ran )) && printf '\e]133;D;%s\a' "$ec"
    __lx_ran=0
    __lx_osc7
    # prompt frameworks (oh-my-zsh, powerlevel10k, starship) rebuild PS1 in their own precmd: put the marks back when they are gone
    [[ $PS1 == *']133;A'* ]] || PS1=$'%{\e]133;A\a%}'"${PS1}"$'%{\e]133;B\a%}'
}
autoload -Uz add-zsh-hook
add-zsh-hook preexec __lx_preexec
add-zsh-hook precmd __lx_precmd
precmd_functions=(__lx_precmd ${precmd_functions:#__lx_precmd})   # first, so $? is still the command's status
