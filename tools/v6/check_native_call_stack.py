#!/usr/bin/env python3
"""Account complete reachable Native protocol frames, including explicit SPI edges.

Product callbacks are NOT claimed to use zero stack: their maximum permissible
extension is reported. Unclassified calls, missing protocol records and stale
source-to-indirect mappings fail closed.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

NODE = re.compile(r'node:\s*\{\s*title:\s*"([^"]+)"\s*label:\s*"([^"]*)"[^}]*\}')
EDGE = re.compile(r'edge:\s*\{\s*sourcename:\s*"([^"]+)"\s*targetname:\s*"([^"]+)"([^}]*)\}')
STACK = re.compile(r'\\n(\d+) bytes \(([^)]+)\)')
LOC = re.compile(r'^(.*):(\d+):(\d+)$')
TOKEN = re.compile(r'[A-Za-z_]\w*|->|[^\s]')
COMMENTS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


class Dependencies:
    """Use actual compiler dependencies; a new policy cannot bless old graphs."""
    def __init__(self, build: Path):
        self.build = build.resolve()
        self.ninja = None
        self.checked = set()

    def check(self, graph: Path):
        if graph in self.checked:
            return
        depfile = graph.with_suffix('.ci.d')
        if depfile.is_file():
            text = re.sub(r'\\\r?\n', ' ', depfile.read_text(encoding='utf-8'))
            tail = text.split(': ', 1)[1]
            deps = [re.sub(r'\\([ #\\])', r'\1', p) for p in re.findall(r'(?:\\.|[^\s])+', tail)]
        else:
            if self.ninja is None:
                cache = (self.build/'CMakeCache.txt').read_text(encoding='utf-8')
                tool = re.search(r'^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$', cache, re.M)
                if not tool or not (self.build/'.ninja_deps').is_file():
                    raise ValueError('missing compiler dependencies for graph: '+str(graph))
                run = subprocess.run([tool[1].strip(), '-C', str(self.build), '-t', 'deps'],
                                     capture_output=True, text=True, encoding='utf-8', timeout=30, check=False)
                if run.returncode:
                    raise ValueError('cannot read Ninja compiler dependencies')
                self.ninja, current = {}, None
                for line in run.stdout.splitlines():
                    match = re.fullmatch(r'(.+): #deps \d+, deps mtime .+ \((VALID|STALE)\)', line)
                    if match:
                        current = match[1] if match[2] == 'VALID' else None
                        if current:
                            self.ninja[current] = []
                    elif current and line.startswith('    '):
                        self.ninja[current].append(line.strip())
            objects = [graph.with_suffix(suffix) for suffix in ('.obj', '.o')]
            objects = [p for p in objects if p.is_file()]
            if len(objects) != 1:
                raise ValueError('missing/ambiguous object for graph: '+str(graph))
            deps = self.ninja.get(objects[0].resolve().relative_to(self.build).as_posix(), [])
        if not deps:
            raise ValueError('empty/stale compiler dependencies: '+str(graph))
        for name in deps:
            path = Path(name)
            if not path.is_absolute():
                path = self.build/path
            if path.stat().st_mtime_ns > graph.stat().st_mtime_ns:
                raise ValueError('graph older than compiler dependency: '+str(path))
        self.checked.add(graph)


def relative(path: str) -> str:
    path = path.replace('\\', '/')
    while '//' in path:
        path = path.replace('//', '/')
    for marker in ('/src/', '/include/'):
        if marker in path:
            return marker[1:]+path.split(marker, 1)[1]
    return path


def compact(text: str) -> str:
    return re.sub(r'\s+', '', text)


def call_expressions(clean: str):
    """Closed C operand grammar: identifiers, members and balanced indexes.

    Cast/dereference-call dialects are not guessed; unknown syntax fails at
    its actual compiler callsite instead of being treated as an external SPI.
    """
    tokens = list(TOKEN.finditer(clean))
    def operand_start(end):
        while end >= 0 and tokens[end][0] == ']':
            depth = 1
            end -= 1
            while end >= 0 and depth:
                depth += (tokens[end][0] == ']') - (tokens[end][0] == '[')
                end -= 1
            if depth:
                return None
        if end < 0 or not re.fullmatch(r'[A-Za-z_]\w*', tokens[end][0]):
            return None
        start = end
        while start >= 2 and tokens[start-1][0] in ('.', '->'):
            start = operand_start(start-2)
            if start is None:
                return None
        return start
    for index, token in enumerate(tokens):
        if token[0] != '(':
            continue
        start = operand_start(index-1)
        if start is not None:
            begin, end = tokens[start].start(), token.start()
            yield begin, end, compact(clean[begin:end])


def expression_at(root: Path, location: str) -> tuple[str, str]:
    match = LOC.fullmatch(location)
    if not match:
        raise ValueError('indirect call has no exact source location: '+location)
    source, row, col = relative(match[1]), int(match[2]), int(match[3])
    path = root/source
    text = path.read_text(encoding='utf-8')
    clean = COMMENTS.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), text)
    rows = clean.splitlines(keepends=True)
    if not 1 <= row <= len(rows) or not 1 <= col <= len(rows[row-1])+1:
        raise ValueError('stale source location: '+location)
    point = sum(map(len, rows[:row-1]))+col-1
    calls = list(call_expressions(clean))
    candidates = [c for c in calls if c[0] <= point <= c[1]]
    if len(candidates) != 1:
        raise ValueError('cannot uniquely parse indirect expression: '+location)
    return source, candidates[0][2]


def analyze(build: Path, root: Path, policy: dict, limit: int) -> list[str]:
    if policy.get('schema') != 1 or limit <= 0:
        raise ValueError('invalid policy schema/limit')
    for source, expected in policy.get('source_sha256', {}).items():
        if hashlib.sha256((root/source).read_bytes()).hexdigest() != expected:
            raise ValueError('source fingerprint changed: '+source)
    for binding in policy.get('bindings', []):
        code = (root/binding['source']).read_text(encoding='utf-8')
        clean = COMMENTS.sub('', code)
        assignments = re.findall(re.escape(binding['object'])+r'\s*\.\s*'+
                                 re.escape(binding['field'])+r'\s*=\s*(\w+)\s*;', clean)
        if assignments != [binding['target']]:
            raise ValueError('stale/noncanonical backend binding: '+binding['field'])
    frames, paths, names, aliases, edges, files = {}, {}, {}, {}, {}, {}
    dependencies = Dependencies(build) if policy.get('require_dependencies') else None
    for path in sorted(build.rglob('*.ci')):
        text = path.read_text(encoding='utf-8', errors='strict')
        # GCC gives static header functions the same textual ID in different
        # translation units. They are distinct functions, never merge them.
        def key(title):
            return str(path)+'::'+title if ':' in title else title
        for match in NODE.finditer(text):
            title, label = match[1], match[2]
            size = STACK.search(label)
            if size is None:  # A declaration; definition must be elsewhere.
                continue
            # GCC Windows ARM paths are raw backslashes: "\native" is NOT
            # a label newline. Split only function-name and final stack suffix.
            name, separator, location = label[:size.start()].partition(r'\n')
            if not separator:
                continue
            loc = LOC.fullmatch(location)
            if not loc:
                continue
            source = relative(loc[1])
            if not source.startswith(('src/', 'include/')):
                continue
            identity = key(title)
            value = (int(size[1]), size[2])
            if identity in frames and frames[identity] != value:
                raise ValueError('conflicting stack record: '+title)
            frames[identity], paths[identity], names[identity] = value, source, name
            files[identity] = path
            aliases.setdefault(source+':'+name, set()).add(identity)
        for match in EDGE.finditer(text):
            location = re.search(r'label:\s*"([^"]+)"', match[3])
            edges.setdefault(key(match[1]), []).append((key(match[2]), match[2], location[1] if location else None))

    def resolve(name):
        options = aliases.get(name, set())
        if len(options) != 1:
            raise ValueError('missing/ambiguous protocol function: '+name)
        return next(iter(options))

    entries = [resolve(name) for name in policy['entries']]
    if not entries:
        raise ValueError('no Native entries')
    indirect = policy.get('indirect', [])
    external = policy.get('external', {})
    graph, direct_graph, external_calls, visiting, reached = {}, {}, {}, set(), set()

    def visit(node):
        if node in visiting:
            raise ValueError('recursive protocol call chain: '+names.get(node, node))
        if node in reached:
            return
        visiting.add(node)
        if dependencies:
            dependencies.check(files[node])
        size, kind = frames[node]
        if kind not in ('static', 'dynamic,bounded'):
            raise ValueError('unbounded stack: '+node)
        graph[node] = set()
        direct_graph[node] = set()
        external_calls[node] = []
        for target, symbol, location in edges.get(node, []):
            if symbol == '__indirect_call':
                source, expression = expression_at(root, location or '')
                mappings = [m for m in indirect if m['source'] == source and
                            compact(m['expression']) == expression and
                            ('caller' not in m or m['caller'] == names[node])]
                if len(mappings) != 1:
                    raise ValueError('unmapped/ambiguous indirect: '+source+':'+expression)
                mapping = mappings[0]
                if bool(mapping.get('targets')) == bool(mapping.get('boundary')):
                    raise ValueError('indirect requires targets XOR boundary: '+expression)
                if mapping.get('targets'):
                    for name in mapping['targets']:
                        child = resolve(name)
                        graph[node].add(child); direct_graph[node].add(child); visit(child)
                else:
                    continuations = set()
                    for name in mapping.get('continuations', []):
                        child = resolve(name)
                        continuations.add(child)
                        graph[node].add(child); visit(child)
                    external_calls[node].append((mapping['boundary'], continuations))
            elif target in frames:
                graph[node].add(target); direct_graph[node].add(target); visit(target)
            elif symbol in external:
                external_calls[node].append((external[symbol], set()))
            else:
                raise ValueError('missing protocol/unclassified external function: '+symbol)
        visiting.remove(node); reached.add(node)

    for entry in entries:
        visit(entry)
    memo = {}

    def longest(node):
        if node not in memo:
            child = max((longest(c) for c in graph[node]), default=(0, []))
            memo[node] = (frames[node][0]+child[0], [node]+child[1])
        return memo[node]

    maximum = max(map(longest, entries))
    if maximum[0] > limit:
        raise ValueError(f'protocol maximum={maximum[0]} > limit={limit}; '+
                         ' -> '.join(f'{names[n]}[{frames[n][0]}]' for n in maximum[1]))
    output = [f'NATIVE_PROTOCOL_STACK_OK functions={len(reached)} maximum={maximum[0]} limit={limit}',
              ' -> '.join(f'{names[n]}[{frames[n][0]}]' for n in maximum[1])]
    # A callback can legally re-enter a protocol API which itself invokes a
    # lock/libc boundary. Its unknown frames are simultaneously live; separate
    # per-category maxima are NOT sufficient. Preserve joint linear budgets.
    symbolic = {}
    def paths_with_boundaries(node):
        if node in symbolic:
            return symbolic[node]
        result = {(): frames[node][0]}
        for child in direct_graph[node]:
            for variables, known in paths_with_boundaries(child).items():
                result[variables] = max(result.get(variables, 0), frames[node][0]+known)
        for label, continuations in external_calls[node]:
            tails = [paths_with_boundaries(c) for c in continuations] or [{(): 0}]
            for tail in tails:
                for variables, known in tail.items():
                    vector = tuple(sorted((*variables, label)))
                    result[vector] = max(result.get(vector, 0), frames[node][0]+known)
        symbolic[node] = result
        return result
    constraints = {}
    for entry in entries:
        for variables, known in paths_with_boundaries(entry).items():
            if variables:
                constraints[variables] = max(constraints.get(variables, 0), known)
    caps = {}
    for variables, known in sorted(constraints.items()):
        remaining = limit-known
        if remaining <= 0:
            raise ValueError('external boundary has no stack budget: '+ '+'.join(variables))
        output.append('CONSTRAINT '+ '+'.join(variables)+f' <= {remaining}')
        for label in set(variables):
            caps[label] = min(caps.get(label, limit), remaining//variables.count(label))
    for label, remaining in sorted(caps.items()):
        output.append(f'BOUNDARY {label} remaining={remaining} PROJECTED_ONLY PRODUCT_STACK_NOT_PROVEN')
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--policy', required=True, type=Path)
    parser.add_argument('--assembly')
    parser.add_argument('--limit-bytes', required=True, type=int)
    args = parser.parse_args()
    try:
        policy = json.loads(args.policy.read_text(encoding='utf-8'))
        if args.assembly:
            shared = policy['shared']
            selected = policy['assemblies'][args.assembly]
            if not shared.get('source_sha256') or not selected.get('bindings'):
                raise ValueError('assembly requires frozen sources and actual Backend bindings')
            policy = {**shared, **selected, 'schema': policy['schema'],
                      'entries': shared.get('entries', [])+selected['entries'],
                      'indirect': shared['indirect']+selected.get('indirect', [])}
        print('\n'.join(analyze(args.build_dir, args.source_root, policy, args.limit_bytes)))
        return 0
    except (ValueError, KeyError, IndexError, TypeError, OSError, UnicodeError, subprocess.SubprocessError) as error:
        print('NATIVE_PROTOCOL_STACK_ERROR '+str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
