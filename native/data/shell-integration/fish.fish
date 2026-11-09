# Lestrix shell integration for fish: marks prompts and command output (OSC 133) and reports the directory (OSC 7).
# Add to ~/.config/fish/config.fish:  source /path/to/fish.fish
status is-interactive; or return
set -q __lx_loaded; and return
set -g __lx_loaded 1

function __lx_osc7 --on-variable PWD
    printf '\e]7;file://%s%s\a' $hostname (string escape --style=url -- $PWD | string replace -a '%2F' '/')
end
function __lx_preexec --on-event fish_preexec
    printf '\e]133;C\a'
end
function __lx_postexec --on-event fish_postexec
    printf '\e]133;D;%s\a' $status
end
function __lx_status; return $argv[1]; end
# Wrap fish_prompt on the first fish_prompt event, so a fish_prompt defined later in config.fish is wrapped too.
function __lx_wrap --on-event fish_prompt
    functions -e __lx_wrap
    functions -q fish_prompt; or return
    functions -c fish_prompt __lx_fish_prompt
    function fish_prompt
        set -l __lx_s $status   # printf below would reset $status to 0 and the user's prompt shows it
        printf '\e]133;A\a'
        __lx_status $__lx_s
        __lx_fish_prompt
        printf '\e]133;B\a'
    end
end
__lx_osc7
