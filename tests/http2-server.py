#!/usr/bin/env python3

import asyncio
from functools import wraps
import os
from secrets import compare_digest
import sys
from urllib.parse import urlparse

from quart import (
    request,
    make_response,
    Quart,
)

app = Quart(__name__)

timer_handle = None
loop = None

def set_timeout():
    global timer_handle

    # ASAN is very slow, just don't have a timer.
    if 'ASAN_OPTIONS' in os.environ:
        return

    if timer_handle:
        timer_handle.cancel()

    # This timeout just prevents a zombie process from
    # running even if a test crashes.
    timer_handle = loop.call_later(20, lambda: sys.exit(0))


@app.route('/')
async def index():
    set_timeout()
    return 'Hello world'

@app.route('/slow')
async def slow():
    set_timeout()
    await asyncio.sleep(1)
    return 'Hello world'

@app.route('/no-content')
async def no_content():
    set_timeout()
    return await make_response('', 204)

@app.route('/large')
async def large():
    set_timeout()

    async def generate_data():
        # Send increasing letters just to aid debugging
        letter = ord('A')
        bytes_pending = 1024 * 24
        while bytes_pending:
            await asyncio.sleep(0.1)
            bytes_pending -= 1024
            string = chr(letter) * 1024
            letter += 1
            yield bytes(string, 'UTF-8')
        yield b'\0'

    return generate_data()

@app.route('/echo_query')
async def echo_query():
    set_timeout()
    url = urlparse(request.url)
    return url.query

@app.route('/echo_post', methods=['POST'])
async def echo_post():
    set_timeout()
    data = await request.get_data()
    return data

@app.route('/auth')
async def auth():
    set_timeout()
    auth = request.authorization

    if (
        auth is not None and 
        auth.type == "basic" and
        auth.username == 'username' and
        compare_digest(auth.password, 'password')
    ):
        return 'Authenticated'

    response = await make_response('Authentication Required')
    response.status_code = 401
    response.headers['WWW-Authenticate'] = 'Basic'
    return response

has_been_misdirected = False

@app.route('/misdirected_request')
async def misdirected_request():
    set_timeout()
    global has_been_misdirected

    if not has_been_misdirected:
        has_been_misdirected = True
        response = await make_response('', 421)
        return response

    return 'Success!'

if __name__ == '__main__':
    loop = asyncio.get_event_loop()
    set_timeout()

    app.run(use_reloader=False, loop=loop,
        certfile='test-cert.pem',
        keyfile='test-key.pem')
