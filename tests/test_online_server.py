"""Transport regressions for the local server; these do not prove in-game P2P works."""
import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from ufc3_server import read_http_request
from ufc3_auth import CLIENT_ID, SERVER_ID, IdentityStore, user_authenticated, post_auth, PS4_PLATFORM
from blaze_tdf import INT, STRING, STRUCT, TIME, Struct, decode_struct, Frame, read_frames, MESSAGE, REPLY, NOTIFICATION
from unittest.mock import patch
import json
import urllib.parse
import ufc3_server


class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.store = IdentityStore()

    def test_ufc_config_includes_easo_url(self):
        payload = ufc3_server.preauth_reply(None)
        fields, consumed = decode_struct(payload)
        self.assertEqual(consumed, len(payload))
        conf = dict(fields['CONF'][1]['CONF'][1][2])
        self.assertEqual(conf['easo_auth_url'], 'https://accounts.ea.com/easo/auth')
        self.assertEqual(conf['projectId'], '0')

    def test_easo_login_returns_required_headers(self):
        request = (b'POST /easo/auth/nv2/auth HTTP/1.0\r\n'
                   b'X-UFC3-Online-Id: Host\r\nX-UFC3-Psn-Id: 1000000001\r\n\r\n')
        answer = ufc3_server.nucleus_answer(request)
        self.assertTrue(answer.startswith(b'HTTP/1.1 200 OK\r\n'))
        for header in (b'EASW-Token: ', b'EASW-Session: ', b'EASW-Nucleus-Display-Name: Host',
                       b'EASW-Token-Expiry-Time: '):
            self.assertIn(header, answer)

    def test_census_subscribe_reply_has_timeouts_and_empty_list(self):
        payload = ufc3_server.census_subscribe(None)
        fields, consumed = decode_struct(payload)
        self.assertEqual(consumed, len(payload))
        self.assertEqual(fields['CNP'], (INT, 15_000_000))
        self.assertEqual(fields['RTMT'], (INT, 60_000_000))
        self.assertNotIn('TDFL', fields)
        self.assertEqual(payload[3], INT)

    def test_fighternet_reply_contains_dummy_card(self):
        payload = ufc3_server.ufc_notifications(None)
        fields, consumed = decode_struct(payload)
        self.assertEqual(consumed, len(payload))
        cards = fields['FTVS'][1][1]
        self.assertEqual(len(cards), 1)
        self.assertEqual(cards[0]['LTLI'], (STRING, 'Dummy'))
        self.assertEqual(cards[0]['NCTY'], (INT, 1))

    def test_friend_list_names_the_other_player(self):
        from ufc3_auth import Account
        linux = Account(1000000002, 11, 'Player_1')
        windows = Account(1000000001, 22, 'myzkW')
        ufc3_server.ONLINE.clear()
        ufc3_server.ONLINE[linux.id] = linux
        ufc3_server.ONLINE[windows.id] = windows
        try:
            payload = ufc3_server.friend_lists(linux)
            fields, consumed = decode_struct(payload)
            self.assertEqual(consumed, len(payload))
            info = fields['LSDT'][1][1][0]
            self.assertEqual(info['ALST'][1]['TYPE'], (INT, 1))
            self.assertEqual(info['BIDS'][1][1], [windows.id])
        finally:
            ufc3_server.ONLINE.clear()

    def test_identity_remains_the_same_through_oauth_and_blaze(self):
        first = self.store.account(123, 'Host')
        second = self.store.account(456, 'Guest')
        self.assertNotEqual(first.id, second.id)
        for account in (first, second):
            client_code = self.store.issue_code(account, CLIENT_ID)
            token = self.store.exchange_token(client_code, CLIENT_ID, account.psn_id)
            self.assertEqual(self.store.token_account(token, account.psn_id), (account, CLIENT_ID))
            self.assertIs(self.store.account(account.psn_id, 'Renamed'), account)
            server_code = self.store.issue_code(account, SERVER_ID)
            request = Struct(AUTH=(STRING, server_code), EXTI=(INT, account.psn_id))
            actual, key, response = self.store.login(request)
            self.assertIs(actual, account)
            fields, consumed = decode_struct(response)
            self.assertEqual(consumed, len(response))
            self.assertEqual(set(fields), {'ANON', 'SESS', 'SPAM', 'UNDR'})
            session = fields['SESS'][1]
            self.assertEqual(fields['SESS'][0], STRUCT)
            self.assertEqual(session['BUID'], (INT, account.id))
            self.assertEqual(session['UID'], (INT, account.id))
            self.assertEqual(session['KEY'], (STRING, key))
            self.assertEqual(session['PDTL'][1]['XREF'], (INT, account.psn_id))
            self.assertEqual(session['PDTL'][1]['DSNM'], (STRING, account.name))
            self.assertEqual(session['PDTL'][1]['PLAT'], (INT, PS4_PLATFORM))
            notice, _ = decode_struct(user_authenticated(account, key))
            self.assertEqual(notice['KEY'], session['KEY'])
            self.assertEqual(notice['BUID'], session['BUID'])
            self.assertEqual(notice['PLAT'], (INT, 20))
            post, _ = decode_struct(post_auth(account))
            self.assertEqual(set(post), {'TELE', 'TICK', 'UROP'})
            self.assertEqual(post['UROP'][1]['UID'], session['UID'])
            self.assertEqual(post['UROP'][1]['TMOP'], (INT, 0))
            self.assertEqual(post['TELE'][1]['PORT'], (INT, 0))
            with self.assertRaises(ValueError):
                self.store.login(request)  # one-use code

    def test_wrong_account_or_client_cannot_redeem_code(self):
        account = self.store.account(123, 'Host')
        code = self.store.issue_code(account, CLIENT_ID)
        for client, psn in ((SERVER_ID, 123), (CLIENT_ID, 456)):
            with self.assertRaises(ValueError):
                self.store.redeem_code(code, client, psn)
        self.assertIs(self.store.redeem_code(code, CLIENT_ID, 123), account)

    def test_expired_code_is_rejected(self):
        account = self.store.account(123, 'Host')
        with patch('ufc3_auth.time.monotonic', return_value=100):
            code = self.store.issue_code(account, SERVER_ID)
        with patch('ufc3_auth.time.monotonic', return_value=401):
            with self.assertRaises(ValueError):
                self.store.redeem_code(code, SERVER_ID, 123)

    def test_bogus_token_and_malformed_login_are_rejected(self):
        with self.assertRaises(ValueError):
            self.store.token_account('bogus', 123)
        with self.assertRaises(ValueError):
            self.store.login(Struct(AUTH=(STRING, 'bogus')))

    def test_game_empty_post_token_grant_and_session_chain(self):
        def call(path, params, body=b''):
            target = path + '?' + urllib.parse.urlencode(params)
            request = (f'POST {target} HTTP/1.0\r\nX-UFC3-Psn-Id: 123\r\n'
                       'X-UFC3-Online-Id: Host\r\nContent-Type: application/x-www-form-urlencoded\r\n'
                       f'Content-Length: {len(body)}\r\n\r\n').encode() + body
            return ufc3_server.nucleus_answer(request)
        def code(client):
            response = call('/connect/auth', {'client_id': client})
            self.assertTrue(response.startswith(b'HTTP/1.1 302'))
            location = next(line for line in response.split(b'\r\n') if line.startswith(b'Location:'))
            return urllib.parse.parse_qs(urllib.parse.urlsplit(location.decode().split(' ', 1)[1]).query)['code'][0]
        with patch.object(ufc3_server, 'IDENTITIES', self.store):
            client_code = code(CLIENT_ID)
            response = call('/connect/token', {'grant_type': 'authorization_code',
                            'client_id': CLIENT_ID, 'code': client_code})
            self.assertTrue(response.startswith(b'HTTP/1.1 200'))
            token = json.loads(response.split(b'\r\n\r\n', 1)[1])['access_token']
            info = json.loads(call('/connect/tokeninfo', {'access_token': token}).split(b'\r\n\r\n', 1)[1])
            account, _, _ = self.store.login(Struct(AUTH=(STRING, code(SERVER_ID)), EXTI=(INT, 123)))
            self.assertEqual(info['persona_id'], account.id)
            replay = call('/connect/token', {'grant_type': 'authorization_code',
                          'client_id': CLIENT_ID, 'code': client_code})
            self.assertTrue(replay.startswith(b'HTTP/1.1 400'))


class HttpTransportTests(unittest.IsolatedAsyncioTestCase):
    async def test_fragmented_headers_and_body(self):
        reader = asyncio.StreamReader()
        task = asyncio.create_task(read_http_request(reader))
        request = b'POST /connect/token HTTP/1.0\r\nContent-Length: 7\r\n\r\ncode=42'
        for byte in request:
            reader.feed_data(bytes([byte]))
            await asyncio.sleep(0)
        self.assertEqual(await task, request)

    async def test_does_not_consume_next_request(self):
        reader = asyncio.StreamReader()
        one = b'GET /connect/auth HTTP/1.0\r\n\r\n'
        reader.feed_data(one + one)
        self.assertEqual(await read_http_request(reader), one)
        self.assertEqual(await read_http_request(reader), one)

    async def test_truncated_body_fails(self):
        reader = asyncio.StreamReader()
        reader.feed_data(b'POST / HTTP/1.0\r\nContent-Length: 10\r\n\r\nx')
        reader.feed_eof()
        with self.assertRaises(asyncio.IncompleteReadError):
            await read_http_request(reader)

    async def test_rejects_ambiguous_or_oversized_lengths(self):
        for headers in (b'Content-Length: 1\r\nContent-Length: 2',
                        b'Content-Length: -1', b'Content-Length: 1048577',
                        b'Transfer-Encoding: chunked'):
            reader = asyncio.StreamReader()
            reader.feed_data(b'POST / HTTP/1.0\r\n' + headers + b'\r\n\r\n')
            with self.assertRaises(ValueError):
                await read_http_request(reader)


class BlazeSessionTests(unittest.IsolatedAsyncioTestCase):
    async def test_login_notifications_and_postauth_over_tcp(self):
        store = IdentityStore()
        account = store.account(123, 'Host')
        code = store.issue_code(account, SERVER_ID)
        async def read_frame(reader):
            head = await asyncio.wait_for(reader.readexactly(16), 2)
            length = int.from_bytes(head[:4], 'big') + int.from_bytes(head[4:6], 'big')
            body = await asyncio.wait_for(reader.readexactly(length), 2)
            return read_frames(bytearray(head + body))[0]
        completed = asyncio.Event()
        async def connection(reader, writer):
            try:
                await ufc3_server.blaze(reader, writer)
            finally:
                completed.set()
        with patch.object(ufc3_server, 'IDENTITIES', store):
            async with await asyncio.start_server(connection, '127.0.0.1', 0) as server:
                port = server.sockets[0].getsockname()[1]
                reader, writer = await asyncio.open_connection('127.0.0.1', port)
                try:
                    from blaze_tdf import encode_struct
                    login = encode_struct(Struct(AUTH=(STRING, code), EXTI=(INT, account.psn_id)))
                    writer.write(Frame(1, 10, 42, MESSAGE, login).encode())
                    await writer.drain()
                    reply = await read_frame(reader)
                    self.assertEqual((reply.msg_type, reply.component, reply.command, reply.msg_num), (REPLY, 1, 10, 42))
                    session = decode_struct(reply.payload)[0]['SESS'][1]
                    self.assertEqual(session['BUID'], (INT, account.id))
                    for command in (8, 2, 5):
                        notice = await read_frame(reader)
                        self.assertEqual((notice.msg_type, notice.component, notice.command), (NOTIFICATION, 0x7802, command))
                        fields = decode_struct(notice.payload)[0]
                        if command == 8:
                            self.assertEqual(fields['KEY'], session['KEY'])
                        elif command == 2:
                            self.assertEqual(fields['USER'][1]['EXID'], (INT, account.psn_id))
                        else:
                            self.assertEqual(fields['ID'], (INT, account.id))
                    writer.write(Frame(9, 8, 43, MESSAGE).encode())
                    await writer.drain()
                    post = await read_frame(reader)
                    self.assertEqual((post.msg_type, post.component, post.command, post.msg_num), (REPLY, 9, 8, 43))
                    self.assertEqual(decode_struct(post.payload)[0]['UROP'][1]['UID'], (INT, account.id))
                finally:
                    await ufc3_server.close_writer(writer)
                await asyncio.wait_for(completed.wait(), 2)


class GameManagerTests(unittest.TestCase):
    def test_matchmaking_setup_has_schema_correct_two_player_roster(self):
        from ufc3_auth import Account
        from blaze_tdf import BLOB, UNION, Union
        import ufc3_games

        ufc3_games.GAMES.clear()
        ufc3_games.SEARCHES.clear()
        host = Account(1000000001, 111, 'WindowsHost')
        guest = Account(1000000002, 222, 'LinuxGuest')
        def network_address(ip):
            return Struct(IP=(INT, int.from_bytes(bytes(map(int, ip.split('.'))), 'big')),
                          MACI=(INT, 0), PORT=(INT, 3659))
        def search(account, peer_ip, ip):
            pnet = Union(2, 'VALU', STRUCT, Struct(INIP=(STRUCT, network_address(ip)),
                                                  EXIP=(STRUCT, network_address(ip)), MACI=(INT, 0)))
            request = Struct(CMGD=(STRUCT, Struct(GVER=(STRING, '1.14'), PNET=(UNION, pnet))),
                             SCNM=(STRING, 'ufcH2HUnranked'))
            return ufc3_games.matchmake(request, account, object(), peer_ip, lambda *_: None)

        try:
            search(host, '192.168.1.129', '192.168.1.129')
            _, notifications = search(guest, '192.168.1.2', '192.168.1.2')
            setups = [frame for _, frame in notifications
                      if frame.component == ufc3_games.GAME_MANAGER
                      and frame.command == ufc3_games.NOTIFY_GAME_SETUP]
            self.assertEqual(len(setups), 2)
            for frame in setups:
                fields, consumed = decode_struct(frame.payload)
                self.assertEqual(consumed, len(frame.payload))
                roster = fields['PROS'][1][1]
                self.assertEqual({p['NAME'][1] for p in roster}, {'WindowsHost', 'LinuxGuest'})
                for player in roster:
                    self.assertIn('EXBL', player)
                    self.assertIn('BLOB', player)
                    self.assertNotIn('EXBB', player)  # EXBB belongs to UserIdentification, not this class.
        finally:
            ufc3_games.GAMES.clear()
            ufc3_games.SEARCHES.clear()


if __name__ == '__main__':
    unittest.main()
