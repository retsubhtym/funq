# -*- coding: utf-8 -*-
"""Pytest plugin that mirrors the classic nose --with-funq integration."""

from __future__ import annotations

import codecs
import logging
import os
from argparse import Namespace
from configparser import ConfigParser

import pytest

from funq import tools
from funq.client import ApplicationRegistry
from funq.screenshoter import ScreenShoter
from funq.testcase import FunqTestCase, MultiFunqTestCase, register_funq_app_registry

LOG = logging.getLogger('pytest.funq')


def _message_with_sep(message):
    sep = '-' * 70
    return sep, message, sep


def _locate_funq():
    return tools.which('funq')


def pytest_addoption(parser):  # pragma: no cover - exercised through pytest
    env = os.environ
    group = parser.getgroup('funq', 'funq pytest plugin options')
    group.addoption('--with-funq', '--funq', action='store_true',
                    dest='funq_enabled', default=False,
                    help='Enable funq integration (launch the configured '
                         'application and wire the client helpers).')
    group.addoption('--funq-conf', action='store',
                    default=env.get('NOSE_FUNQ_CONF') or 'funq.conf',
                    help='Funq configuration file (defaults to funq.conf) '
                         '[NOSE_FUNQ_CONF].')
    group.addoption('--funq-gkit', action='store',
                    default=env.get('NOSE_FUNQ_GKIT') or 'default',
                    help='Select the graphic toolkit profile to load '
                         '[NOSE_FUNQ_GKIT].')
    gkit_file = os.path.join(os.path.dirname(os.path.realpath(__file__)),
                             'aliases-gkits.conf')
    group.addoption('--funq-gkit-file', action='store',
                    default=env.get('NOSE_FUNQ_GKIT_FILE') or gkit_file,
                    help='Override the file defining graphic toolkits '
                         f'default: {gkit_file} [NOSE_FUNQ_GKIT_FILE].')
    group.addoption('--funq-attach-exe', action='store',
                    default=env.get('NOSE_FUNQ_ATTACH_EXE') or _locate_funq(),
                    help='Path to the funq executable used for library '
                         'injection [NOSE_FUNQ_ATTACH_EXE].')
    group.addoption('--funq-trace-tests', action='store',
                    default=env.get('NOSE_FUNQ_TRACE_TESTS'),
                    help='Optional file pathway used to log test start/end '
                         '[NOSE_FUNQ_TRACE_TESTS].')
    group.addoption('--funq-trace-tests-encoding', action='store',
                    default=env.get('NOSE_FUNQ_TRACE_TESTS_ENCODING') or 'utf-8',
                    help='Encoding of --funq-trace-tests '
                         '[NOSE_FUNQ_TRACE_TESTS_ENCODING].')
    group.addoption('--funq-screenshot-folder', action='store',
                    default=(env.get('NOSE_FUNQ_SCREENSHOT_FOLDER')
                             or os.path.realpath('screenshot-errors')),
                    help='Folder where screenshots are stored after '
                         'failures [NOSE_FUNQ_SCREENSHOT_FOLDER].')
    group.addoption('--funq-snooze-factor', action='store', type=float,
                    default=float(env.get('NOSE_FUNQ_SNOOZE_FACTOR', 1.0)),
                    help='Factor applied on every internal timeout '
                         '[NOSE_FUNQ_SNOOZE_FACTOR].')


def pytest_configure(config):  # pragma: no cover - runtime hook
    if not config.getoption('funq_enabled'):
        return
    state = _FunqPytestState(config)
    state.configure()
    config._funq_state = state  # type: ignore[attr-defined]


def pytest_unconfigure(config):  # pragma: no cover - runtime hook
    state = getattr(config, '_funq_state', None)
    if state is None:
        return
    state.teardown()
    delattr(config, '_funq_state')


def pytest_runtest_setup(item):  # pragma: no cover - runtime hook
    state = getattr(item.config, '_funq_state', None)
    if state is not None:
        state.on_test_start(item)


def pytest_runtest_teardown(item, nextitem):  # pragma: no cover - runtime hook
    del nextitem  # unused
    state = getattr(item.config, '_funq_state', None)
    if state is not None:
        state.on_test_finish(item)


@pytest.hookimpl(hookwrapper=True)  # pragma: no cover - runtime hook
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    state = getattr(item.config, '_funq_state', None)
    if state is not None:
        state.handle_report(item, report)


class _FunqPytestState:
    """Runtime helper that mirrors the behaviour of the nose plugin."""

    def __init__(self, config):
        self._config = config
        self._options = None
        self._trace_tests = None
        self._trace_encoding = 'utf-8'
        self._screenshoter = None

    def configure(self):
        options = self._build_options()
        conf_file = os.path.realpath(options.funq_conf)
        if not os.path.isfile(conf_file):
            raise pytest.UsageError(
                f'Missing funq configuration file: {conf_file}')
        parser = ConfigParser()
        parser.read([conf_file])
        app_registry = ApplicationRegistry()
        app_registry.register_from_conf(parser, options)
        register_funq_app_registry(app_registry)
        self._trace_tests = options.funq_trace_tests
        self._trace_encoding = options.funq_trace_tests_encoding
        self._screenshoter = ScreenShoter(options.funq_screenshot_folder)
        tools.SNOOZE_FACTOR = float(options.funq_snooze_factor)
        self._options = options

    def teardown(self):
        self._options = None
        self._trace_tests = None
        self._screenshoter = None

    def on_test_start(self, item):
        message = f"Starting test `{self._describe_item(item)}`"
        self._log_message(message)

    def on_test_finish(self, item):
        message = f"Ending test `{self._describe_item(item)}`"
        self._log_message(message)

    def handle_report(self, item, report):
        if report.when != 'call' or not report.failed:
            return
        if not self._screenshoter:
            return
        testcase = getattr(item, '_testcase', None)
        if testcase is None:
            return
        if isinstance(testcase, MultiFunqTestCase):
            if testcase.__app_config__:
                for name, appconf in testcase.__app_config__.items():
                    if appconf.screenshot_on_error:
                        self._screenshoter.take_screenshot(
                            testcase.funq[name],
                            f"{testcase.id()} [{name}]")
        elif isinstance(testcase, FunqTestCase):
            appconf = getattr(testcase, '__app_config__', None)
            if appconf and appconf.screenshot_on_error:
                self._screenshoter.take_screenshot(testcase.funq,
                                                   testcase.id())

    def _log_message(self, message):
        lines = _message_with_sep(message)
        for line in lines:
            LOG.info(line)
        if self._trace_tests:
            with codecs.open(self._trace_tests, 'a', self._trace_encoding) as f:
                f.write('\n'.join(lines))
                f.write('\n')

    def _describe_item(self, item):
        testcase = getattr(item, '_testcase', None)
        if testcase is not None and hasattr(testcase, 'id'):
            return testcase.id()
        return item.nodeid

    def _build_options(self):
        if self._options is not None:
            return self._options
        opts = Namespace()
        pytest_options = self._config.getoption
        opts.funq_conf = pytest_options('funq_conf')
        opts.funq_gkit = pytest_options('funq_gkit')
        opts.funq_gkit_file = pytest_options('funq_gkit_file')
        opts.funq_attach_exe = pytest_options('funq_attach_exe')
        opts.funq_trace_tests = pytest_options('funq_trace_tests')
        opts.funq_trace_tests_encoding = pytest_options('funq_trace_tests_encoding')
        opts.funq_screenshot_folder = pytest_options('funq_screenshot_folder')
        opts.funq_snooze_factor = pytest_options('funq_snooze_factor')
        return opts
