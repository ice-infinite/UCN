#!/usr/bin/env python3
"""Exercise native protocol indirect-call accounting through its CLI."""
import json
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import generate_native_call_stack_policy as generator
import check_native_call_stack as checker

CHECKER = Path(__file__).with_name('check_native_call_stack.py')


class NativeCallStackTests(unittest.TestCase):
    def run_gate(self, root_size=256, target_size=512, *, missing=False,
                 unknown=False, cycle=False, kind='static', expression='b->send',
                 declared='b->send', external=False, continuation=False,
                 changed_hash=False, wrong_binding=False, stale=False,
                 dependencies=False, windows_source=False, direct_symbol=None,
                 allow_direct=False, assembly=False, inherited_cycle=False, wrapper=False):
        with tempfile.TemporaryDirectory(prefix='ucn-native-chain-') as directory:
            root = Path(directory)
            (root/'src').mkdir()
            source_name = 'src/native.c' if windows_source else 'src/a.c'
            source = root/source_name
            graph_source = str(source).replace('/', '\\') if windows_source else source.as_posix()
            source.write_text('void start(void) { '+expression+'(); }\nvoid target(void) {}\n')
            names = ['start'] if missing else ['start', 'target']
            sizes = [root_size] if missing else [root_size, target_size]
            records = [f'node: {{ title: "{name}" label: "{name}\\n{graph_source}:{i+1}:1\\n{size} bytes ({kind})" }}'
                       for i, (name, size) in enumerate(zip(names, sizes))]
            records.append(f'edge: {{ sourcename: "start" targetname: "__indirect_call" label: "{graph_source}:1:20" }}')
            if cycle:
                records.append('edge: { sourcename: "target" targetname: "start" }')
            if continuation:
                records.append('edge: { sourcename: "target" targetname: "lock" }')
            if direct_symbol:
                records.append(f'edge: {{ sourcename: "start" targetname: "{direct_symbol}" }}')
            (root/'calls.ci').write_text('\n'.join(records))
            policy = {'schema': 1, 'entries': [source_name+':start'],
                      'indirect': [] if unknown else [{'source': source_name, 'expression': declared,
                          **({'boundary': 'driver', **({'continuations': [source_name+':target']} if continuation else {})}
                             if external else {'targets': [source_name+':target']})}],
                      'external': {'lock': 'lock'}}
            if allow_direct:
                policy['external'][direct_symbol] = 'compiler-runtime'
            if changed_hash:
                policy['source_sha256'] = {'src/a.c': hashlib.sha256(source.read_bytes()+b'x').hexdigest()}
            if wrong_binding:
                policy['bindings'] = [{'source': 'src/a.c', 'object': 'b->port', 'field': 'send', 'target': 'start'}]
            if dependencies:
                policy['require_dependencies'] = True
                (root/'calls.ci.d').write_text('calls.o: '+source.as_posix()+'\n')
            if stale:
                stat = (root/'calls.ci').stat()
                os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns+2_000_000_000))
            if assembly:
                source.write_text(source.read_text()+'void factory(void) { b->port.send = target; }\n')
                selected = {key: policy[key] for key in ('entries', 'indirect')}
                selected['bindings'] = [{'source': source_name, 'object': 'b->port', 'field': 'send', 'target': 'target'}]
                policy = {'schema': 1, 'shared': {
                    'source_sha256': {source_name: hashlib.sha256(source.read_bytes()).hexdigest()},
                    'external': policy['external'], 'indirect': []},
                    'assemblies': {'h1': selected, 'dynamic': {'inherits': 'h1', 'entries': [], 'indirect': []}}}
                if inherited_cycle:
                    selected['inherits'] = 'dynamic'
                if wrapper:
                    target_mapping = selected['indirect'].pop()
                    policy['shared']['indirect'] = [{
                        'source': source_name, 'expression': declared, 'boundary': 'crypto'}]
                    policy['assemblies']['dynamic']['indirect'] = [{
                        **target_mapping, 'replaces_boundary': 'crypto'}]
            (root/'policy.json').write_text(json.dumps(policy))
            return subprocess.run([sys.executable, str(CHECKER), '--build-dir', str(root),
                '--source-root', str(root), '--policy', str(root/'policy.json'), '--limit-bytes', '768'],
                text=True, capture_output=True, check=False) if not assembly else subprocess.run(
                    [sys.executable, str(CHECKER), '--build-dir', str(root), '--source-root', str(root),
                     '--policy', str(root/'policy.json'), '--assembly', 'dynamic', '--limit-bytes', '768'],
                    text=True, capture_output=True, check=False)

    def test_indirect_target_counts_at_exact_limit(self):
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('maximum=768', result.stdout)

    def test_indirect_one_byte_over_limit_rejected(self):
        self.assert_rejected(self.run_gate(target_size=513))

    def assert_rejected(self, result):
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn('NATIVE_PROTOCOL_STACK_ERROR', result.stdout)
        self.assertNotIn('Traceback', result.stderr)

    def test_indexed_and_multiline_member_call(self):
        for expression in ('b->ports[slot].send', 'b\n -> send'):
            result = self.run_gate(expression=expression, declared=expression)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_windows_native_path_is_not_a_newline_delimiter(self):
        result = self.run_gate(windows_source=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('maximum=768', result.stdout)

    def test_missing_target_unmapped_call_and_changed_source_rejected(self):
        for args in ({'missing': True}, {'unknown': True}, {'expression': 'b->new_send'}):
            with self.subTest(args=args):
                self.assert_rejected(self.run_gate(**args))

    def test_recursion_and_unbounded_rejected(self):
        self.assert_rejected(self.run_gate(cycle=True))
        self.assert_rejected(self.run_gate(kind='dynamic'))

    def test_bounded_dynamic_uses_the_full_compiler_bound(self):
        result = self.run_gate(kind='dynamic,bounded')
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('maximum=768', result.stdout)
        self.assert_rejected(self.run_gate(kind='dynamic,bounded', target_size=513))

    def test_only_explicit_runtime_symbol_is_a_conditional_boundary(self):
        policy = json.loads(CHECKER.with_name('native_call_stack_policy.json').read_text(encoding='utf-8'))
        self.assertEqual(policy['shared']['external']['__stack_chk_fail'], 'compiler-runtime')
        result = self.run_gate(direct_symbol='__stack_chk_fail', allow_direct=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('BOUNDARY compiler-runtime remaining=512', result.stdout)
        self.assertIn('PRODUCT_STACK_NOT_PROVEN', result.stdout)
        self.assert_rejected(self.run_gate(direct_symbol='unclassified_runtime'))

    def test_cortex_division_runtime_is_budgeted(self):
        policy = json.loads(CHECKER.with_name('native_call_stack_policy.json').read_text(encoding='utf-8'))
        external = policy['shared']['external']
        for symbol in ('__aeabi_uldivmod', '__aeabi_ldivmod'):
            with self.subTest(symbol=symbol):
                self.assertEqual(external.get(symbol), 'compiler-runtime')
                result = self.run_gate(direct_symbol=symbol, allow_direct=True)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn('CONSTRAINT compiler-runtime <= 512', result.stdout)
                self.assertIn('PRODUCT_STACK_NOT_PROVEN', result.stdout)
        self.assert_rejected(self.run_gate(direct_symbol='__aeabi_unclassified'))

    def test_external_boundary_is_conditional_budget_not_zero_stack_claim(self):
        result = self.run_gate(external=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('BOUNDARY driver remaining=512', result.stdout)
        self.assertIn('PRODUCT_STACK_NOT_PROVEN', result.stdout)

    def test_zero_remaining_external_budget_is_not_pass(self):
        self.assert_rejected(self.run_gate(root_size=768, external=True))

    def test_callback_protocol_continuation_and_joint_product_budget(self):
        result = self.run_gate(root_size=256, target_size=256, external=True, continuation=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('maximum=512', result.stdout)
        self.assertIn('CONSTRAINT driver+lock <= 256', result.stdout)
        self.assertIn('PROJECTED_ONLY', result.stdout)
        self.assert_rejected(self.run_gate(target_size=513, external=True, continuation=True))

    def test_frozen_hash_and_factory_binding_mismatch(self):
        self.assert_rejected(self.run_gate(changed_hash=True))
        self.assert_rejected(self.run_gate(wrong_binding=True))

    def test_compiler_dependencies_reject_old_graph_even_if_policy_refreshed(self):
        self.assertEqual(self.run_gate(dependencies=True).returncode, 0)
        self.assert_rejected(self.run_gate(stale=True, dependencies=True))

    def test_dynamic_assembly_inherits_actual_backend_targets_not_a_boundary(self):
        result = self.run_gate(assembly=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('maximum=768', result.stdout)
        self.assert_rejected(self.run_gate(assembly=True, target_size=513))

    def test_assembly_inheritance_cycle_fails_closed_without_traceback(self):
        self.assert_rejected(self.run_gate(assembly=True, inherited_cycle=True))

    def test_dynamic_inventory_cannot_omit_a_leaf_or_foundation_entry(self):
        for entry in ('src/runtime/ucn_native_reauth.c:ucn_i_native_reauth_handoff',
                      'src/persistence/ucn_persistence.c:ucn_i_persistence_step'):
            policy = json.loads(generator.POLICY.read_text(encoding='utf-8'))
            policy['assemblies']['dynamic']['entries'].remove(entry)
            with self.assertRaisesRegex(ValueError, 'inventory changed: dynamic'):
                generator.expected(policy)

    def test_public_management_entry_is_required_in_both_static_assemblies(self):
        for assembly in ('h0', 'h1'):
            policy = json.loads(generator.POLICY.read_text(encoding='utf-8'))
            policy['assemblies'][assembly]['entries'].remove(
                'src/native/ucn_native.c:ucn_native_backend_lifecycle_range')
            with self.assertRaisesRegex(ValueError, 'inventory changed: '+assembly):
                generator.expected(policy)

    def test_dynamic_cannot_replace_the_actual_backend_with_a_boundary(self):
        policy = json.loads(generator.POLICY.read_text(encoding='utf-8'))
        policy['assemblies']['dynamic']['indirect'] = [{
            'source': 'src/native/ucn_native.c', 'expression': 'n->backend->send',
            'boundary': 'not-a-product-boundary'}]
        with self.assertRaisesRegex(ValueError, 'without replacement'):
            generator.expected(policy)

    def test_dynamic_verifier_wrapper_is_a_real_protocol_target(self):
        policy = json.loads(generator.POLICY.read_text(encoding='utf-8'))
        resolved = checker.resolve_assembly(policy, 'dynamic')
        mappings = [m for m in resolved['indirect'] if m['expression'] == 'call->verify_identity']
        self.assertEqual(len(mappings), 1)
        self.assertEqual(mappings[0].get('targets'), [
            'src/runtime/ucn_native_reauth_durability.c:verify_with_budget'])
        self.assertNotIn('boundary', mappings[0])
        result = self.run_gate(assembly=True, wrapper=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('maximum=768', result.stdout)
        self.assert_rejected(self.run_gate(assembly=True, wrapper=True, target_size=513))


if __name__ == '__main__':
    unittest.main()
