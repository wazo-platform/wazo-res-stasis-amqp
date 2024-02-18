# Copyright 2019-2024 The Wazo Authors  (see the AUTHORS file)
# SPDX-License-Identifier: GPL-3.0-or-later

import logging
import os

import ari as ari_client
import pytest
from hamcrest import (
    any_of,
    assert_that,
    calling,
    empty,
    has_entries,
    has_entry,
    has_item,
    has_items,
    is_not,
    not_,
    only_contains,
)
from wazo_test_helpers import until
from wazo_test_helpers.asset_launching_test_case import AssetLaunchingTestCase
from wazo_test_helpers.bus import BusClient
from wazo_test_helpers.hamcrest.raises import raises

log_level = logging.DEBUG if os.environ.get('TEST_LOGS') == 'verbose' else logging.INFO
logging.basicConfig(level=log_level)

app_name_key = 'applicationName'

subscribe_args = {app_name_key: 'newstasisapplication'}


class AssetLauncher(AssetLaunchingTestCase):

    assets_root = os.path.join(os.path.dirname(__file__), '..', 'assets')
    asset = 'amqp'
    service = 'ari_amqp'


@pytest.fixture()
def ari(request):
    AssetLauncher.kill_containers()
    AssetLauncher.rm_containers()
    AssetLauncher.asset = request.param
    AssetLauncher.launch_service_with_asset()
    ari_port = AssetLauncher.service_port(5039, 'ari_amqp')
    ari_url = f'http://127.0.0.1:{ari_port}'
    client = until.return_(
        ari_client.connect, ari_url, 'wazo', 'wazo', timeout=5, interval=0.1
    )

    # necessary because RabbitMQ starts much more slowly, so module fails to load automatically
    AssetLauncher.docker_exec(
        ['asterisk', '-rx', 'module load res_stasis_amqp.so'],
        service_name='ari_amqp',
    )
    AssetLauncher.docker_exec(
        ['asterisk', '-rx', 'module load res_ari_amqp.so'],
        service_name='ari_amqp',
    )
    AssetLauncher.docker_exec(
        ['asterisk', '-rx', 'core set debug 5'],
        service_name='ari_amqp',
    )
    yield client
    AssetLauncher.kill_containers()


@pytest.mark.parametrize('ari', ['headers'], indirect=True)
def test_stasis_amqp_events_headers(ari):
    real_app = 'A'
    parasite_app = 'B'
    ari.amqp.stasisSubscribe(applicationName=real_app)
    ari.amqp.stasisSubscribe(applicationName=parasite_app)

    assert_that(ari.applications.list(), has_item(has_entry('name', real_app)))
    assert_that(ari.applications.list(), has_item(has_entry('name', parasite_app)))

    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-headers',
        exchange_type='headers',
    )

    assert bus_client.is_up()

    accumulator = bus_client.accumulator(
        headers={
            'category': 'stasis',
            'application_name': real_app,
            'x-match': 'all',
        }
    )
    parasite_accumulator = bus_client.accumulator(
        headers={
            'category': 'stasis',
            'application_name': parasite_app,
            'x-match': 'all',
        }
    )

    ari.channels.originate(endpoint='local/3000@default', app=real_app)
    ari.channels.originate(endpoint='local/3000@default', app=parasite_app)

    def event_received():
        events = accumulator.accumulate(with_headers=True)
        assert_that(
            events,
            only_contains(
                has_entries(
                    headers=has_entries(application_name=real_app, category='stasis'),
                    message=has_entries(data=has_entries(application=real_app)),
                ),
            ),
        )

        assert_that(
            parasite_accumulator.accumulate(),
            only_contains(has_entries(data=has_entries(application=is_not(real_app)))),
        )

    until.assert_(event_received, timeout=5)

    def event_received():
        assert_that(
            accumulator.accumulate(),
            only_contains(has_entries(data=has_entries(application=real_app))),
        )

        assert_that(
            parasite_accumulator.accumulate(),
            only_contains(has_entries(data=has_entries(application=is_not(real_app)))),
        )

    until.assert_(event_received, timeout=5)


@pytest.mark.parametrize('ari', ['filter-events'], indirect=True)
def test_stasis_amqp_app_exclude_events(ari):
    app = 'MyApp'
    ari.amqp.stasisSubscribe(applicationName=app)
    assert_that(ari.applications.list(), has_item(has_entry('name', app)))

    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-headers',
        exchange_type='headers',
    )
    assert bus_client.is_up()

    headers = {
        'category': 'stasis',
        'application_name': app,
        'x-match': 'all',
    }
    accumulator = bus_client.accumulator(headers=headers)

    ari.channels.originate(endpoint='local/3000@default', app=app)

    def event_received():
        events = accumulator.accumulate(with_headers=True)
        # NOTE: StasisStart is sent after ChannelDialplan and ChannelStateChange
        assert_that(
            events,
            has_items(has_entries(headers=has_entries(name='StasisStart'))),
        )

    until.assert_(event_received, timeout=5)

    assert_that(
        accumulator.accumulate(with_headers=True),
        not_(
            any_of(
                has_items(has_entries(headers=has_entries(name='ChannelDialplan'))),
                has_items(has_entries(headers=has_entries(name='ChannelStateChange'))),
            ),
        ),
    )


@pytest.mark.parametrize('ari', ['headers'], indirect=True)
def test_stasis_amqp_ami_events_headers(ari):
    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-headers',
        exchange_type='headers',
    )

    accumulator = bus_client.accumulator(
        headers={
            'category': 'ami',
            'name': 'DeviceStateChange',
            'x-match': 'all',
        }
    )

    ari.channels.originate(
        endpoint='local/3000@default', extension='1000', context='default'
    )

    def event_received():
        events = accumulator.accumulate(with_headers=True)
        assert_that(
            events,
            has_item(
                has_entries(
                    headers=has_entries(category='ami', name='DeviceStateChange'),
                    message=has_entries(
                        name='DeviceStateChange',
                        data=has_entries(
                            Event='DeviceStateChange',
                            Device='Local/3000@default',
                        ),
                    ),
                ),
            ),
        )

    until.assert_(event_received, timeout=5)


@pytest.mark.parametrize('ari', ['topic'], indirect=True)
def test_stasis_amqp_ami_events_topic(ari):
    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-topic',
        exchange_type='topic',
    )

    accumulator = bus_client.accumulator('ami.devicestatechange')

    ari.channels.originate(
        endpoint='local/3000@default', extension='1000', context='default'
    )

    def event_received():
        events = accumulator.accumulate()
        assert_that(
            events,
            has_item(
                has_entries(
                    name='DeviceStateChange',
                    data=has_entries(
                        Event='DeviceStateChange',
                        Device='Local/3000@default',
                    ),
                ),
            ),
        )

    until.assert_(event_received, timeout=5)


@pytest.mark.parametrize('ari', ['no-publish'], indirect=True)
def test_stasis_amqp_ami_events_disabled(ari):
    ari.amqp.stasisSubscribe(applicationName='myapp')

    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-headers',
        exchange_type='headers',
    )

    ami_accumulator = bus_client.accumulator(
        headers={
            'category': 'ami',
            'name': 'DeviceStateChange',
            'x-match': 'all',
        }
    )
    app_accumulator = bus_client.accumulator(
        headers={
            'category': 'stasis',
            'application_name': 'myapp',
            'x-match': 'all',
        }
    )

    ari.channels.originate(endpoint='local/3000@default', app='myapp')

    def event_received():
        events = app_accumulator.accumulate(with_headers=True)
        assert_that(
            events,
            only_contains(
                has_entries(
                    headers=has_entries(application_name='myapp', category='stasis'),
                ),
            ),
        )

    until.assert_(event_received, timeout=5)

    events = ami_accumulator.accumulate(with_headers=True)
    assert_that(events, empty())


@pytest.mark.parametrize('ari', ['filter-events'], indirect=True)
def test_stasis_amqp_ami_exclude_events(ari):
    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-headers',
        exchange_type='headers',
    )

    ami_accumulator = bus_client.accumulator(
        headers={
            'category': 'ami',
            'name': 'DeviceStateChange',
            'x-match': 'all',
        }
    )

    ari.channels.originate(
        endpoint='local/3000@default', extension='1000', context='default'
    )

    events = ami_accumulator.accumulate(with_headers=True)
    assert_that(events, empty())


@pytest.mark.parametrize('ari', ['no-publish'], indirect=True)
def test_stasis_amqp_channel_events_disabled(ari):
    ari.amqp.stasisSubscribe(applicationName='myapp')

    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-headers',
        exchange_type='headers',
    )

    channel_accumulator = bus_client.accumulator(
        headers={
            'category': 'ami',
            'name': 'Dial',
            'x-match': 'all',
        }
    )
    app_accumulator = bus_client.accumulator(
        headers={
            'category': 'stasis',
            'application_name': 'myapp',
            'x-match': 'all',
        }
    )

    ari.channels.originate(endpoint='local/3000@default', app='myapp')

    def event_received():
        events = app_accumulator.accumulate(with_headers=True)
        assert_that(
            events,
            only_contains(
                has_entries(
                    headers=has_entries(application_name='myapp', category='stasis'),
                ),
            ),
        )

    until.assert_(event_received, timeout=5)

    events = channel_accumulator.accumulate(with_headers=True)
    assert_that(events, empty())


@pytest.mark.parametrize('ari', ['headers'], indirect=True)
def test_stasis_amqp_channel_events_headers(ari):
    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-headers',
        exchange_type='headers',
    )

    accumulator = bus_client.accumulator(
        headers={
            'category': 'stasis',
            'name': 'Dial',
            'x-match': 'all',
        }
    )

    ari.channels.originate(
        endpoint='local/3000@default', extension='1000', context='default'
    )

    def event_received():
        events = accumulator.accumulate(with_headers=True)
        assert_that(
            events,
            has_item(
                has_entries(
                    headers=has_entries(
                        category='stasis',
                        name='Dial',
                    ),
                    message=has_entries(
                        name='Dial',
                        data=has_entries(
                            type='Dial',
                            dialstring='3000@default',
                        ),
                    ),
                )
            ),
        )

    until.assert_(event_received, timeout=5)


@pytest.mark.parametrize('ari', ['topic'], indirect=True)
def test_stasis_amqp_channel_events_topic(ari):
    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-topic',
        exchange_type='topic',
    )

    accumulator = bus_client.accumulator('stasis.channel.dial')

    ari.channels.originate(
        endpoint='local/3000@default', extension='1000', context='default'
    )

    def event_received():
        events = accumulator.accumulate()
        assert_that(
            events,
            has_item(
                has_entries(
                    name='Dial',
                    data=has_entries(
                        type='Dial',
                        dialstring='3000@default',
                    ),
                ),
            ),
        )

    until.assert_(event_received, timeout=5)


@pytest.mark.parametrize('ari', ['filter-events'], indirect=True)
def test_stasis_amqp_channel_exclude_events(ari):
    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-headers',
        exchange_type='headers',
    )

    accumulator = bus_client.accumulator(
        headers={
            'category': 'stasis',
            'x-match': 'all',
        }
    )

    ari.channels.originate(
        endpoint='local/3000@default', extension='1000', context='default'
    )

    def event_received():
        events = accumulator.accumulate(with_headers=True)
        # NOTE: ChannelHangupRequest is sent after Dial
        assert_that(
            events,
            has_items(has_entries(headers=has_entries(name='ChannelHangupRequest'))),
        )

    until.assert_(event_received, timeout=5)

    assert_that(
        accumulator.accumulate(with_headers=True),
        not_(has_items(has_entries(headers=has_entries(name='Dial')))),
    )


@pytest.mark.parametrize('ari', ['topic'], indirect=True)
def test_stasis_amqp_events_topic(ari):
    real_app = 'A'
    parasite_app = 'B'
    ari.amqp.stasisSubscribe(applicationName=real_app)
    ari.amqp.stasisSubscribe(applicationName=parasite_app)

    assert_that(ari.applications.list(), has_item(has_entry('name', real_app)))
    assert_that(ari.applications.list(), has_item(has_entry('name', parasite_app)))

    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-topic',
        exchange_type='topic',
    )

    assert bus_client.is_up()

    accumulator = bus_client.accumulator('stasis.app.a')
    parasite_accumulator = bus_client.accumulator('stasis.app.b')

    ari.channels.originate(endpoint='local/3000@default', app=real_app)
    ari.channels.originate(endpoint='local/3000@default', app=parasite_app)

    def event_received():
        events = accumulator.accumulate()
        assert_that(
            events,
            only_contains(
                has_entries(data=has_entries(application=real_app)),
            ),
        )

        assert_that(
            parasite_accumulator.accumulate(),
            only_contains(
                has_entries(data=has_entries(application=is_not(real_app))),
            ),
        )

    until.assert_(event_received, timeout=5)

    def event_received():
        assert_that(
            accumulator.accumulate(),
            only_contains(has_entries(data=has_entries(application=real_app))),
        )

        assert_that(
            parasite_accumulator.accumulate(),
            only_contains(has_entries(data=has_entries(application=is_not(real_app)))),
        )

    until.assert_(event_received, timeout=5)


@pytest.mark.parametrize('ari', ['headers'], indirect=True)
def test_stasis_amqp_events_bad_routing(ari):
    real_app = 'A'
    parasite_app = 'B'
    ari.amqp.stasisSubscribe(applicationName=real_app)
    ari.amqp.stasisSubscribe(applicationName=parasite_app)

    bus_client = BusClient.from_connection_fields(
        port=AssetLauncher.service_port(5672, 'rabbitmq'),
        exchange_name='wazo-headers',
        exchange_type='headers',
    )

    assert bus_client.is_up()

    accumulator = bus_client.accumulator(
        headers={
            'category': 'stasis',
            'application_name': parasite_app,
            'x-match': 'all',
        }
    )

    ari.channels.originate(endpoint='local/3000@default', app=real_app.lower())

    def event_received():
        assert_that(accumulator.accumulate(), empty())

    until.assert_(event_received, timeout=5)


@pytest.mark.parametrize('ari', ['headers'], indirect=True)
def test_app_subscribe(ari):
    assert_that(
        calling(ari.amqp.stasisSubscribe).with_args(**subscribe_args),
        not_(raises(Exception)),
    )

    assert_that(
        ari.applications.list(),
        has_item(has_entry('name', subscribe_args[app_name_key])),
    )


@pytest.mark.parametrize('ari', ['headers'], indirect=True)
def test_app_unsubscribe(ari):
    app_name = 'my-test-app'
    ari.amqp.stasisSubscribe(applicationName=app_name)

    assert_that(
        calling(ari.amqp.stasisUnsubscribe).with_args(applicationName=app_name),
        not_(raises(Exception)),
    )

    applications = ari.applications.list()
    assert_that(applications, not_(has_item(has_entries(name=app_name))))
