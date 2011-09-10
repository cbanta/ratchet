
require "package"

module("ratchet.socketpad", package.seeall)
local class = getfenv()
__index = class

-- {{{ new()
function new(socket)
    local self = {}
    setmetatable(self, class)

    self.socket = socket

    self.recv_buffer = ''
    self.send_buffer = ''

    return self
end
-- }}}

-- {{{ recv_once()
local function recv_once(self)
    local data, err = self.socket:recv()
    if not data then
        return data, err
    end

    self.recv_buffer = self.recv_buffer .. data
    return data
end
-- }}}

-- {{{ recv_until_bytes()
local function recv_until_bytes(self, bytes)
    while #self.recv_buffer < bytes do
        local data, err = recv_once(self)
        if not data then
            return nil, err
        end
    end

    local ret = self.recv_buffer:sub(1, bytes)
    self.recv_buffer = self.recv_buffer:sub(bytes+1)
    return ret
end
-- }}}

-- {{{ recv_until_string()
local function recv_until_string(self, str)
    local escaped_str = str:gsub('([%^%$%(%)%%%.%[%]%*%+%-%?])', '%1')
    local pattern = escaped_str .. '()'

    while true do
        local end_i = self.recv_buffer:match(pattern)
        if end_i then
            local ret = self.recv_buffer:sub(1, end_i-1)
            self.recv_buffer = self.recv_buffer:sub(end_i)
            return ret
        end

        local data, err = recv_once(self)
        if not data then
            return nil, err
        end
    end
end
-- }}}

-- {{{ recv()
function recv(self, through)
    if type(through) == "string" then
        return recv_until_string(self, through)
    elseif type(through) == "number" then
        return recv_until_bytes(self, through)
    else
        return nil, "Argument must be a number or string."
    end
end
-- }}}

-- {{{ recv_remaining()
function recv_remaining(self)
    local ret = self.recv_buffer
    self.recv_buffer = ''

    return ret
end
-- }}}

-- {{{ peek()
function peek(self)
    return self.recv_buffer
end
-- }}}

-- {{{ send()
function send(self, data, more)
    self.send_buffer = self.send_buffer .. data
    if not more then
        local to_send = self.send_buffer
        self.send_buffer = ''

        return self.socket:send(to_send)
    end
end
-- }}}

-- vim:foldmethod=marker:sw=4:ts=4:sts=4:et: