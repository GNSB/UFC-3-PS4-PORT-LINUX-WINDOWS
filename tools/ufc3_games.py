# SPDX-License-Identifier: GPL-2.0-or-later
"""GameManager (Blaze component 4) for a two-player LAN fight: one game, its host and a guest.

The fight itself is the game's own DirtySDK peer-to-peer traffic between the two PCs; the
server only creates the game, tells each player the other's address and relays state changes.

Tag names are the ones UFC's eboot knows (its tag pool); the message shapes follow Blaze 13
(BF4) and BlazeSDK, whose decoders skip members a class does not have.
"""
import itertools
import socket
import uuid

from blaze_tdf import INT, STRING, STRUCT, LIST, MAP, UNION, BLOB, NOTIFICATION, Frame, Struct, Union

GAME_MANAGER = 4
USER_SESSIONS = 0x7802

# Commands (client -> server).
CREATE_GAME, DESTROY_GAME, ADVANCE_GAME_STATE, SET_GAME_SETTINGS = 0x1, 0x2, 0x3, 0x4
SET_GAME_ATTRIBUTES, JOIN_GAME, REMOVE_PLAYER = 0x7, 0x9, 0xB
START_MATCHMAKING, CANCEL_MATCHMAKING, FINALIZE_GAME_CREATION = 0xD, 0xE, 0xF
START_MATCHMAKING_SCENARIO, CANCEL_MATCHMAKING_SCENARIO, LEAVE_GAME_BY_GROUP = 0x10, 0x11, 0x16
MESH_ENDPOINTS_CONNECTED, MESH_ENDPOINTS_DISCONNECTED, MESH_ENDPOINTS_CONNECTION_LOST = 0x41, 0x42, 0x43
# (UFC's own command table: the eboot's GameManager command-name switch.)

# Notifications (server -> client).
# (UFC's notification-name switch: 0xB is NotifyMatchmakingSessionConnectionValidated; Blaze 15
# has no client NotifyMatchmakingFinished, NotifyGameSetup with a MatchmakingSetupContext is it.)
NOTIFY_MATCHMAKING_FAILED, NOTIFY_MATCHMAKING_CONNECTION_VALIDATED = 0xA, 0xB
NOTIFY_GAME_SETUP, NOTIFY_PLAYER_JOINING, NOTIFY_PLAYER_JOIN_COMPLETED = 0x14, 0x15, 0x1E
NOTIFY_PLAYER_REMOVED, NOTIFY_PLATFORM_HOST_INITIALIZED = 0x28, 0x47
NOTIFY_GAME_ATTRIB_CHANGE, NOTIFY_GAME_STATE_CHANGE, NOTIFY_GAME_SETTINGS_CHANGE = 0x50, 0x64, 0x6E
NOTIFY_GAME_PLAYER_STATE_CHANGE = 0x74
UPDATE_PRIMARY_EXTERNAL_SESSION = 0xB1

# GameState.
INITIALIZING, PRE_GAME, IN_GAME = 1, 0x82, 0x83
# PlayerState.
RESERVED, QUEUED, ACTIVE_CONNECTING, ACTIVE_MIGRATING, ACTIVE_CONNECTED = 0, 1, 2, 3, 4

_game_ids = itertools.count(1)
_scenario_ids = itertools.count(1)


def ip_int(text: str) -> int:
    return int.from_bytes(socket.inet_aton(text), 'big')


def lan_address() -> str:
    """This PC's LAN address (the server and the Linux player share it)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(('192.168.1.1', 9))  # sends nothing
            return s.getsockname()[0]
        except OSError:
            return '127.0.0.1'


def reachable_pnet(pnet, peer_ip: str):
    """The player's network address as the other PC must use it.

    The game reports the address netctl gave it. A player on the server's PC reports loopback,
    which the other PC cannot reach; use the server's LAN address for it instead.
    """
    if not isinstance(pnet, Union) or pnet.member == 0x7f or not isinstance(pnet.value, dict):
        return pnet
    pair = Struct(pnet.value)
    seen = lan_address() if peer_ip.startswith('127.') else peer_ip
    for tag in ('INIP', 'EXIP'):
        kind, addr = pair.get(tag, (STRUCT, Struct()))
        addr = Struct(addr)
        port = addr.get('PORT', (INT, 0))[1]
        ip = addr.get('IP', (INT, 0))[1]
        if not ip or (ip >> 24) == 127:
            ip = ip_int(seen)
        if not port:
            port = Struct(pair.get('INIP', (STRUCT, Struct()))[1]).get('PORT', (INT, 0))[1]
        addr['IP'] = (INT, ip)
        addr['PORT'] = (INT, port)
        addr.setdefault('MACI', (INT, 0))
        pair[tag] = (STRUCT, addr)
    return Union(pnet.member, pnet.tag, pnet.type, pair)


class Player:
    def __init__(self, account, writer, pnet, slot):
        self.account, self.writer, self.pnet, self.slot = account, writer, pnet, slot
        self.state = ACTIVE_CONNECTING
        self.matched = False  # joinedViaMatchmaking
        self.search = 0  # its matchmaking scenario/session id when matched

    def replicated(self, game) -> Struct:
        """ReplicatedGamePlayer."""
        a = self.account
        return Struct(
            BLOB=(BLOB, b''), CONG=(INT, a.id), CSID=(INT, self.slot), DSUI=(INT, 0),
            EXBL=(BLOB, b''), EXID=(INT, getattr(a, 'psn_id', 0) or 0),
            GID=(INT, game.gid), JFPS=(INT, 0), JVMM=(INT, int(self.matched)), LOC=(INT, 1701729619), NAME=(STRING, a.name),
            NASP=(STRING, 'cem_ea_id'), PATT=(MAP, (STRING, STRING, [])), PID=(INT, a.id),
            PNET=(UNION, self.pnet), SID=(INT, self.slot), SLOT=(INT, 0), STAT=(INT, self.state),
            TIDX=(INT, 0), TIME=(INT, 0), UID=(INT, a.id))


class Game:
    def __init__(self, request: Struct, host: Player):
        self.gid = next(_game_ids)
        self.uuid = str(uuid.uuid4())
        self.request = request
        settings = Struct(request.get('GMCD', (STRUCT, Struct()))[1])
        self.settings = settings
        self.attributes = list(settings.get('ATTR', (MAP, (STRING, STRING, [])))[1][2])
        self.gset = settings.get('GSET', (INT, 0))[1]
        self.capacity = settings.get('PMAX', (INT, 2))[1] or 2
        self.topology = settings.get('NTOP', (INT, 130))[1]
        self.presence = settings.get('PRES', (INT, 1))[1]
        self.voip = settings.get('VOIP', (INT, 0))[1]
        self.gtype = request.get('GTYP', (STRING, ''))[1]
        common = Struct(request.get('CMGD', (STRUCT, Struct()))[1])
        self.version = common.get('GVER', (STRING, ''))[1]
        self.state = INITIALIZING
        self.host = host
        self.players = [host]
        self.links = set()  # (reporting player id, connected player id) from meshEndpointsConnected

    def data(self) -> Struct:
        """ReplicatedGameData."""
        # HostInfo (UFC reflection): CONG connection group, CSID connection slot, HPID, HSES, HSLT.
        h = self.host.account.id
        host = Struct(CONG=(INT, h), CSID=(INT, self.host.slot), HPID=(INT, h), HSES=(INT, h),
                      HSLT=(INT, self.host.slot))
        pcap = [self.capacity, 0, 0, 0]
        return Struct(
            ADMN=(LIST, (INT, [self.host.account.id])),
            ATTR=(MAP, (STRING, STRING, self.attributes)),
            CAP=(LIST, (INT, pcap)), CRIT=(MAP, (STRING, STRING, [])),
            DSTR=(MAP, (STRING, STRING, [])), ESNM=(STRING, ''),
            GID=(INT, self.gid), GMRG=(INT, 0), GNAM=(STRING, self.host.account.name),
            GPVH=(INT, 0x1A2B3C4D), GSET=(INT, self.gset), GSID=(INT, self.gid),
            GSTA=(INT, self.state), GTYP=(STRING, self.gtype), GURL=(STRING, ''),
            GVER=(STRING, self.version),
            HNET=(LIST, (UNION, [self.host.pnet])), HSES=(INT, self.host.account.id),
            IGNO=(INT, 0), MCAP=(INT, self.capacity),
            NQOS=(STRUCT, Struct(DBPS=(INT, 0), NATT=(INT, 0), UBPS=(INT, 0))),
            NRES=(INT, 0), NTOP=(INT, self.topology), PGID=(STRING, self.uuid),
            PGSR=(BLOB, b''), PHST=(STRUCT, host), PRES=(INT, self.presence),
            PSAS=(STRING, 'local'), QCAP=(INT, 0), SEED=(INT, self.gid * 7919),
            TCAP=(INT, 0), THST=(STRUCT, host),
            TIDS=(LIST, (INT, [65534])), UUID=(STRING, self.uuid),
            VOIP=(INT, self.voip), VSTR=(STRING, self.version))

    def setup(self, player: Player, context: int = 0, reason=None) -> bytes:
        """NotifyGameSetup for `player`: the game, its roster and why the player is in it.

        GameSetupReason member 0 (DatalessSetupContext): DCTX 0 = created, 1 = joined.
        """
        from blaze_tdf import encode_struct
        # GameSetupReason: each member has its own tag (DLSC 0, IJGS 1, IMSC 2, MMSC 3, RDSC 4);
        # a member under another tag does not decode and the client gets an empty context.
        reason = reason or Union(0, 'DLSC', STRUCT, Struct(DCTX=(INT, context)))
        return encode_struct(Struct(
            GAME=(STRUCT, self.data()),
            PROS=(LIST, (STRUCT, [p.replicated(self) for p in self.players])),
            MNAM=(STRING, 'GameMode'), QOSV=(INT, 0), REAS=(UNION, reason),
            TELM=(INT, 30_000_000)))  # qosTelemetryInterval (µs); unset, clients report every frame

    def find(self, account_id):
        return next((p for p in self.players if p.account.id == account_id), None)


GAMES = {}  # gid -> Game


def open_game_for(account_id):
    """A game with a free seat that this account is not already in."""
    for game in GAMES.values():
        if len(game.players) < game.capacity and not game.find(account_id):
            return game
    return None


def game_of(account_id):
    return next((g for g in GAMES.values() if g.find(account_id)), None)


def leave(account_id, log):
    """The account disconnected: remove it, and the game if it was the host."""
    drop_search(account_id, log)
    game = game_of(account_id)
    if not game:
        return []
    from blaze_tdf import encode_struct
    out = []
    player = game.find(account_id)
    game.players.remove(player)
    removed = encode_struct(Struct(CNTX=(INT, 0), GID=(INT, game.gid), PID=(INT, account_id), REAS=(INT, 0)))
    for other in game.players:
        out.append((other.writer, Frame(GAME_MANAGER, NOTIFY_PLAYER_REMOVED, 0, NOTIFICATION, removed)))
    if player is game.host or not game.players:
        GAMES.pop(game.gid, None)
        log(f'game {game.gid} closed ({player.account.name} left)')
    return out


def handle(command, request, account, writer, peer_ip, log):
    """Returns (reply payload, [(writer, Frame)] notifications) or None when not handled."""
    from blaze_tdf import encode_struct
    request = request or Struct()
    notes = []
    if command == CREATE_GAME:
        old = game_of(account.id)
        if old:
            notes += leave(account.id, log)
        settings = Struct(request.get('PLJD', (STRUCT, Struct()))[1])
        common = Struct(request.get('CMGD', (STRUCT, Struct()))[1])
        pnet = reachable_pnet(common.get('PNET', (UNION, Union()))[1], peer_ip)
        host = Player(account, writer, pnet, 0)
        game = Game(request, host)
        GAMES[game.gid] = game
        log(f'game {game.gid} created by {account.name} ({game.gtype}, {game.capacity} seats, '
            f'attributes {dict(game.attributes)}, address {describe_pnet(pnet)})')
        notes.append((writer, Frame(GAME_MANAGER, NOTIFY_GAME_SETUP, 0, NOTIFICATION, game.setup(host, 0))))
        return encode_struct(Struct(GID=(INT, game.gid))), notes  # CreateGameResponse

    if command == START_MATCHMAKING_SCENARIO:
        return matchmake(request, account, writer, peer_ip, log)

    if command == CANCEL_MATCHMAKING_SCENARIO:
        drop_search(account.id, log)
        return b'', notes

    if command in (JOIN_GAME, START_MATCHMAKING):
        gid = request.get('GID', (INT, 0))[1]
        game = GAMES.get(gid) or open_game_for(account.id)
        if not game:
            log(f'{account.name} wants a game; none is open yet')
            return None
        own = game_of(account.id)
        if own and own is not game:
            notes += leave(account.id, log)  # Quick Fight's own empty session
        common = Struct(request.get('CMGD', (STRUCT, Struct()))[1])
        pnet = reachable_pnet(common.get('PNET', request.get('PNET', (UNION, Union())))[1], peer_ip)
        player = Player(account, writer, pnet, len(game.players))
        game.players.append(player)
        log(f'{account.name} joins game {game.gid} of {game.host.account.name} '
            f'(address {describe_pnet(pnet)})')
        # Each side must know the other user (UserSessions UserAdded + UserUpdated, with the
        # network address) before the roster names them; BF4's join flow does the same.
        from ufc3_auth import user_added, user_updated
        for other in game.players:
            if other is not player:
                for to, who in ((other, player), (player, other)):
                    notes.append((to.writer, Frame(USER_SESSIONS, 2, 0, NOTIFICATION, user_added(who.account, who.pnet))))
                    notes.append((to.writer, Frame(USER_SESSIONS, 5, 0, NOTIFICATION, user_updated(who.account))))
        joining = encode_struct(Struct(GID=(INT, game.gid), PDAT=(STRUCT, player.replicated(game))))
        for other in game.players:
            if other is not player:
                notes.append((other.writer, Frame(GAME_MANAGER, NOTIFY_PLAYER_JOINING, 0, NOTIFICATION, joining)))
        if command == JOIN_GAME:
            notes.append((writer, Frame(GAME_MANAGER, NOTIFY_GAME_SETUP, 0, NOTIFICATION, game.setup(player, 1))))
            return encode_struct(Struct(GID=(INT, game.gid), JGS=(INT, 0))), notes
        # Matchmaking found the host's game: MatchmakingSetupContext (union member 3),
        # result SUCCESS_JOINED_EXISTING_GAME (2).
        msid = next(_scenario_ids)
        found = Struct(FIT=(INT, 100), MAXF=(INT, 100), MSCD=(INT, msid), MSID=(INT, msid), RSLT=(INT, 2),
                       USID=(INT, account.id))
        reason = Union(3, 'MMSC', STRUCT, found)
        notes.append((writer, Frame(GAME_MANAGER, NOTIFY_GAME_SETUP, 0, NOTIFICATION, game.setup(player, reason=reason))))
        log(f'matchmaking {msid}: {account.name} -> game {game.gid}')
        return encode_struct(Struct(MSID=(INT, msid))), notes

    game = game_of(account.id)
    if not game:
        return None
    gid = Struct(GID=(INT, game.gid))

    if command == ADVANCE_GAME_STATE:
        game.state = request.get('GSTA', (INT, game.state))[1]
        log(f'game {game.gid} state -> {game.state:#x}')
        change = encode_struct(Struct(GID=(INT, game.gid), GSTA=(INT, game.state)))
        for p in game.players:
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_GAME_STATE_CHANGE, 0, NOTIFICATION, change)))
        return b'', notes

    if command == FINALIZE_GAME_CREATION:
        log(f'game {game.gid} finalized by {account.name}')
        init = encode_struct(Struct(GID=(INT, game.gid), PHID=(INT, game.host.account.id), PHST=(INT, 0)))
        for p in game.players:
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_PLATFORM_HOST_INITIALIZED, 0, NOTIFICATION, init)))
        return b'', notes

    if command == SET_GAME_ATTRIBUTES:
        changed = dict(request.get('ATTR', (MAP, (STRING, STRING, [])))[1][2])
        merged = dict(game.attributes); merged.update(changed)
        game.attributes = sorted(merged.items())
        body = encode_struct(Struct(ATTR=(MAP, (STRING, STRING, sorted(changed.items()))), GID=(INT, game.gid)))
        for p in game.players:
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_GAME_ATTRIB_CHANGE, 0, NOTIFICATION, body)))
        return b'', notes

    if command == SET_GAME_SETTINGS:
        game.gset = request.get('GSET', (INT, game.gset))[1]
        body = encode_struct(Struct(ATTR=(INT, game.gset), GID=(INT, game.gid)))
        for p in game.players:
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_GAME_SETTINGS_CHANGE, 0, NOTIFICATION, body)))
        return b'', notes

    if command == MESH_ENDPOINTS_CONNECTED:
        # A member reports its peer connection (TCG: the other end's connection group). Once both
        # ends of a pair report it, the joining player is in: ACTIVE_CONNECTED + join completed.
        target = request.get('TCG', (INT, (0, 0, 0)))[1]
        other = target[2] if isinstance(target, tuple) else 0
        log(f'mesh: {account.name} connected to connection group {target}')
        if other and other != account.id:
            game.links.add((account.id, other))
        for p in game.players:
            if p is game.host or p.state == ACTIVE_CONNECTED:
                continue
            h = game.host.account.id
            if (p.account.id, h) in game.links and (h, p.account.id) in game.links:
                p.state = ACTIVE_CONNECTED
                log(f'game {game.gid}: {p.account.name} joined (peer connection up both ways)')
                for who in (game.host, p):
                    state = encode_struct(Struct(GID=(INT, game.gid), PID=(INT, who.account.id),
                                                 STAT=(INT, ACTIVE_CONNECTED)))
                    for q in game.players:
                        notes.append((q.writer, Frame(GAME_MANAGER, NOTIFY_GAME_PLAYER_STATE_CHANGE, 0,
                                                      NOTIFICATION, state)))
                done = encode_struct(Struct(GID=(INT, game.gid), PID=(INT, p.account.id)))
                for q in game.players:
                    notes.append((q.writer, Frame(GAME_MANAGER, NOTIFY_PLAYER_JOIN_COMPLETED, 0, NOTIFICATION, done)))
                # Matchmaking ends for its members once their connections are validated; until
                # then the client keeps its scenario open and the lobby does not start.
                for q in game.players:
                    if not q.search:
                        continue
                    results = Struct(ALST=(LIST, (INT, [])), FCNT=(INT, 0), GIDL=(LIST, (INT, [])),
                                     NTOP=(INT, game.topology), TIER=(INT, 0))
                    valid = encode_struct(Struct(CONV=(STRUCT, results), DISP=(INT, 1), GID=(INT, game.gid),
                                                 MSCD=(INT, q.search), MSID=(INT, q.search), QSVR=(INT, 0),
                                                 USID=(INT, q.account.id)))
                    notes.append((q.writer, Frame(GAME_MANAGER, NOTIFY_MATCHMAKING_CONNECTION_VALIDATED, 0,
                                                  NOTIFICATION, valid)))
                    log(f'matchmaking {q.search}: {q.account.name} connection validated, search finished')
        return b'', notes

    if command == UPDATE_PRIMARY_EXTERNAL_SESSION:
        return encode_struct(Struct(GID=(INT, game.gid), USEI=(INT, account.id))), notes

    if command in (MESH_ENDPOINTS_DISCONNECTED, MESH_ENDPOINTS_CONNECTION_LOST, LEAVE_GAME_BY_GROUP):
        log(f'game {game.gid}: {account.name} sent {command:#x} {dict(request)}')
        return b'', notes

    if command in (DESTROY_GAME, REMOVE_PLAYER, CANCEL_MATCHMAKING, CANCEL_MATCHMAKING_SCENARIO):
        notes += leave(account.id, log)
        return b'', notes

    return None


SEARCHES = []  # players waiting in startMatchmakingScenario, oldest first

# Game settings for a Quick Fight made by matchmaking, as UFC's own Quick Fight session creates
# them (createGame from the host: gameType4, GameMode 4, peer-to-peer full mesh, 2 seats).
MATCH_GAME = Struct(
    CMGD=(STRUCT, Struct(GGTY=(INT, 0), GVER=(STRING, '1.14'))),
    GMCD=(STRUCT, Struct(ATTR=(MAP, (STRING, STRING, [('GameMode', '4'), ('MaxRounds', '5')])),
                         GSET=(INT, 295940), NTOP=(INT, 130), PMAX=(INT, 2), PMIN=(INT, 1),
                         PRES=(INT, 1), VOIP=(INT, 0))),  # VOIP_DISABLED: no voice mesh to wait for
    GTYP=(STRING, 'gameType4'))


class Search:
    def __init__(self, account, writer, pnet, msid, scenario):
        self.account, self.writer, self.pnet, self.msid, self.scenario = account, writer, pnet, msid, scenario


def drop_search(account_id, log):
    for search in [s for s in SEARCHES if s.account.id == account_id]:
        SEARCHES.remove(search)
        log(f'matchmaking {search.msid}: {search.account.name} stopped searching')


def matchmake(request, account, writer, peer_ip, log):
    """startMatchmakingScenario: pair two searching players in a new game.

    The first searcher waits. When a second one arrives the server creates the game with the
    first as its host (MatchmakingSetupContext RSLT 0, SUCCESS_CREATED_GAME) and the second
    joining it (RSLT 1, SUCCESS_JOINED_NEW_GAME); both get NotifyGameSetup.
    """
    from blaze_tdf import encode_struct
    from ufc3_auth import user_added, user_updated
    notes = []
    request = request or Struct()
    common = Struct(request.get('CMGD', (STRUCT, Struct()))[1])
    version = common.get('GVER', (STRING, '1.14'))[1]
    pnet = reachable_pnet(common.get('PNET', (UNION, Union()))[1], peer_ip)
    scenario = request.get('SCNM', (STRING, ''))[1]
    msid = next(_scenario_ids)
    if game_of(account.id):
        notes += leave(account.id, log)  # Quick Fight's own private session
    drop_search(account.id, log)
    other = next((s for s in SEARCHES if s.account.id != account.id), None)
    if not other:
        SEARCHES.append(Search(account, writer, pnet, msid, scenario))
        log(f'matchmaking {msid}: {account.name} searching {scenario or "(scenario)"} '
            f'(address {describe_pnet(pnet)}); waiting for an opponent')
        return encode_struct(Struct(MSID=(INT, msid))), notes
    SEARCHES.remove(other)
    settings = Struct(MATCH_GAME)
    settings['CMGD'] = (STRUCT, Struct(GGTY=(INT, 0), GVER=(STRING, version)))
    host = Player(other.account, other.writer, other.pnet, 0)
    host.state = ACTIVE_CONNECTED
    game = Game(settings, host)
    guest = Player(account, writer, pnet, 1)
    host.matched = guest.matched = True
    game.players.append(guest)
    GAMES[game.gid] = game
    log(f'matchmaking: {other.account.name} (host, {describe_pnet(other.pnet)}) vs {account.name} '
        f'({describe_pnet(pnet)}) in new game {game.gid}')
    for to, who in ((host, guest), (guest, host)):
        notes.append((to.writer, Frame(USER_SESSIONS, 2, 0, NOTIFICATION, user_added(who.account, who.pnet))))
        notes.append((to.writer, Frame(USER_SESSIONS, 5, 0, NOTIFICATION, user_updated(who.account))))
    host.search, guest.search = other.msid, msid
    for player, search_id, result in ((host, other.msid, 0), (guest, msid, 1)):
        # Blaze 15 carries the scenario id (MSCD) besides the session id (MSID); the client
        # finds its own scenario by it and leaves a game set up for an unknown one.
        found = Struct(FIT=(INT, 100), MAXF=(INT, 100), MSCD=(INT, search_id), MSID=(INT, search_id),
                       RSLT=(INT, result), USID=(INT, player.account.id))
        reason = Union(3, 'MMSC', STRUCT, found)
        notes.append((player.writer, Frame(GAME_MANAGER, NOTIFY_GAME_SETUP, 0, NOTIFICATION,
                                           game.setup(player, reason=reason))))
    return encode_struct(Struct(MSID=(INT, msid))), notes


def describe_pnet(pnet) -> str:
    try:
        pair = pnet.value
        out = []
        for tag in ('INIP', 'EXIP'):
            addr = pair[tag][1]
            ip = addr['IP'][1]
            out.append(f'{tag} {socket.inet_ntoa(ip.to_bytes(4, "big"))}:{addr["PORT"][1]}')
        return ', '.join(out)
    except Exception:
        return repr(pnet)
