function zero_index(index)
    return index + 1
end

function error(text)
    gamescope.log(gamescope.log_priority.error, text)
end

function warn(text)
    gamescope.log(gamescope.log_priority.warning, text)
end

function info(text)
    gamescope.log(gamescope.log_priority.info, text)
end

function debug(text)
    gamescope.log(gamescope.log_priority.debug, text)
end

function command(args)
    gamescope.command(args)
end