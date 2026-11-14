#!/usr/bin/env python3
"""Freeze/check source and canonical Native vtable bindings after reviewed edits.

CI uses --check; --write is an explicit developer operation, not an automatic
approval of new callback targets. Declared targets must match the actual factory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT/'tools/v6/native_call_stack_policy.json'
SOURCES = {'h0': 'src/runtime/ucn_native_session.c', 'h1': 'src/runtime/ucn_native_routed.c'}
FIELDS = {'binding', 'local_view', 'preflight', 'start', 'stop', 'step', 'send', 'poll', 'validate'}
DYNAMIC_SOURCES = ('ucn_native_join.c', 'ucn_native_join_durability.c',
    'ucn_native_reauth.c', 'ucn_native_reauth_durability.c',
    'ucn_native_capability.c', 'ucn_native_dynamic.c')
DYNAMIC_SUPPORT_SOURCES = ('src/persistence/ucn_persistence.c',)
REAUTH_WRAPPER = {'source': 'src/security/ucn_security_session.c',
    'expression': 'call->verify_identity', 'replaces_boundary': 'crypto',
    'targets': ['src/runtime/ucn_native_reauth_durability.c:verify_with_budget']}
REAUTH_BINDING = {'source': 'src/runtime/ucn_native_reauth_durability.c',
    'object': 'call', 'member': '->', 'field': 'verify_identity', 'target': 'verify_with_budget'}


def exported(source):
    code = (ROOT/source).read_text(encoding='utf-8')
    return {source+':'+n for n in re.findall(
        r'^(?:ucn_result_t|bool|void)\s+(ucn_\w+)\(', code, re.M)}

CALLBACK_APIS = ('ucn_publisher_create', 'ucn_subscription_create', 'ucn_native_check_schema',
    'ucn_publish', 'ucn_native_send', 'ucn_publisher_close', 'ucn_subscription_close',
    'ucn_subscription_quiescent', 'ucn_native_source_fence', 'ucn_native_stop', 'ucn_native_status')


def expected(policy):
    policy['shared']['require_dependencies'] = True
    native = (ROOT/'src/native/ucn_native.c').read_text(encoding='utf-8')
    public = {'src/native/ucn_native.c:'+n for n in re.findall(r'^ucn_result_t\s+(ucn_\w+)\(', native, re.M)}
    for assembly, source in SOURCES.items():
        selected = policy['assemblies'][assembly]
        entries = set(selected['entries'])
        wanted = public | exported(source)
        if entries != wanted:
            raise ValueError('Native entry inventory changed: '+assembly)
        bindings = {}
        for mapping in selected['indirect']:
            if mapping['source'] != 'src/native/ucn_native.c':
                continue
            field = mapping['expression'].split('->')[-1]
            targets = mapping['targets']
            if field not in FIELDS or len(targets) != 1 or not targets[0].startswith(source+':'):
                raise ValueError('invalid declared Backend target: '+field)
            target = targets[0].split(':')[-1]
            if field in bindings and bindings[field] != target:
                raise ValueError('conflicting Backend field: '+field)
            bindings[field] = target
        if set(bindings) != FIELDS:
            raise ValueError('incomplete Backend vtable: '+assembly)
        code = (ROOT/source).read_text(encoding='utf-8')
        actual = re.findall(r'b->port\.(\w+)\s*=\s*(\w+)\s*;', code)
        for field, target in bindings.items():
            if [v for k, v in actual if k == field] != [target]:
                raise ValueError('factory no longer implements declared field: '+field)
        selected['bindings'] = [{'source': source, 'object': 'b->port', 'field': field, 'target': target}
                                for field, target in sorted(bindings.items())]
    dynamic = policy['assemblies']['dynamic']
    wanted = set().union(*(exported(s) for s in
        tuple('src/runtime/'+s for s in DYNAMIC_SOURCES)+DYNAMIC_SUPPORT_SOURCES))
    if dynamic.get('inherits') != 'h1' or set(dynamic['entries']) != wanted:
        raise ValueError('Native entry inventory changed: dynamic')
    if dynamic.get('bindings') != [REAUTH_BINDING] or dynamic.get('indirect') != [REAUTH_WRAPPER]:
        raise ValueError('dynamic must inherit the reviewed h1 Backend without replacement')
    # The specialized Security verifier is bound by this Root to a protocol
    # budget wrapper, not directly to an opaque product crypto callback.
    code = (ROOT/REAUTH_BINDING['source']).read_text(encoding='utf-8')
    if re.findall(r'call->verify_identity\s*=\s*(\w+)\s*;', code) != ['verify_with_budget']:
        raise ValueError('Reauth verifier wrapper factory binding changed')
    # These are the documented callback-safe Native APIs. They are protocol
    # continuations, not hidden in an external application's stack allowance.
    for mapping in policy['shared']['indirect']:
        if mapping['source'] == 'src/native/ucn_native.c' and mapping['expression'] == 'fn':
            mapping['continuations'] = ['src/native/ucn_native.c:'+n for n in CALLBACK_APIS]
        if mapping['source'] == 'src/adapter/ucn_adapter.c' and mapping['expression'] in ('staging->submit', 'staging->cancel'):
            mapping['continuations'] = ['src/adapter/ucn_adapter.c:'+n for n in
                ('ucn_i_adapter_tx_complete', 'ucn_i_adapter_rx_publish', 'ucn_i_adapter_link_event')]
    paths = sorted({*(ROOT/'src').rglob('*.c'), *(ROOT/'src').rglob('*.h'), *(ROOT/'include').rglob('*.h'), ROOT/'CMakeLists.txt'})
    policy['shared']['source_sha256'] = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    return policy


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    try:
        before = POLICY.read_text(encoding='utf-8')
        after = expected(json.loads(before))
        if args.write:
            POLICY.write_text(json.dumps(after, indent=2)+'\n', encoding='utf-8')
        elif json.loads(before) != after:
            raise ValueError('policy/source changed; review mappings, then explicitly regenerate --write')
        print('NATIVE_STACK_POLICY_OK sources='+str(len(after['shared']['source_sha256'])))
        return 0
    except (ValueError, KeyError, OSError) as error:
        print('NATIVE_STACK_POLICY_ERROR '+str(error))
        return 1


if __name__ == '__main__':
    sys.exit(main())
