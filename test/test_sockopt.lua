require "ratchet"

function ctx1(host, port)
    local rec = ratchet.socket.prepare_tcp(host, port)
    local socket = ratchet.socket.new(rec.family, rec.socktype, rec.protocol)
    socket.SO_REUSEADDR = true
    socket:bind(rec.addr)
    socket:listen()

    assert(socket.SO_ACCEPTCONN == true, "SO_ACCEPTCONN != true")

    assert(socket.SO_REUSEADDR == true, "SO_REUSEADDR != true")
    socket.SO_REUSEADDR = false
    assert(socket.SO_REUSEADDR == false, "SO_REUSEADDR != false")
    socket.SO_REUSEADDR = true
end

function ctx2(host, port)
    local rec = ratchet.socket.prepare_tcp(host, port)
    local socket = ratchet.socket.new(rec.family, rec.socktype, rec.protocol)

    assert(socket.SO_ACCEPTCONN == false, "SO_ACCEPTCONN != false")

    -- Linux kernel doubles whatever value you set to SO_SND/RCVBUF.
    socket.SO_SNDBUF = 1024
    assert(socket.SO_SNDBUF == 2048, "SO_SNDBUF (" .. socket.SO_SNDBUF .. ") != 2048")
end

kernel = ratchet.new(function ()
    ratchet.thread.attach(ctx1, "localhost", 10025)
    ratchet.thread.attach(ctx2, "localhost", 10025)
end)
kernel:loop()

-- vim:foldmethod=marker:sw=4:ts=4:sts=4:et:
