"""Create a separate Solace v3 config from a complete, user-saved baseline.

No game process access. Missing fields and differing formats are errors.
Unlisted fields, custom data and numeric override binding metadata are retained.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent


def field_key(category, name):
    data = category.encode('utf-8')[:120] + b'.' + name.encode('utf-8')[:120]
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f'{value & 0xFFFFFFFF:08x}'


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'Duplicate JSON key: {key}')
        result[key] = value
    return result


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=unique_object)


def check_value(value, spec, label):
    kind = spec['type_id']
    valid = False
    if kind == 0:
        valid = isinstance(value, dict) and type(value.get('v')) is bool and 'b' in value
        if valid and value['b'] is not None:
            binding = value['b']
            valid = (isinstance(binding, dict) and type(binding.get('k')) is int
                     and type(binding.get('m')) is int and type(binding.get('l')) is bool)
    elif kind == 1:
        valid = type(value) is bool
    elif kind in (2, 3):
        valid = type(value) is int and (kind != 3 or 0 <= value <= 255)
    elif kind == 4:
        valid = type(value) in (int, float)
    elif kind == 7:
        valid = (isinstance(value, list) and all(type(x) is bool for x in value)
                 and len(value) == len(spec['elements']))
    if not valid:
        raise ValueError(f'Unexpected value shape for {label}: {value!r}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    config_dir = Path(os.environ['APPDATA']) / 'solace_int' / 'configs'
    parser.add_argument('--base', type=Path, default=config_dir / 'hvh_base.json')
    parser.add_argument('--output', type=Path, default=config_dir / 'hvh_balanced.json')
    parser.add_argument('--plan', type=Path, default=HERE / 'hvh-balanced.plan.json')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    baseline = args.base.resolve(strict=True)
    output = args.output.resolve()
    report_path = output.with_suffix('.changes.txt')
    if output == baseline or output.exists() or report_path.exists():
        raise ValueError('Choose a new output name; existing files are never overwritten.')
    baseline_bytes = baseline.read_bytes()
    base = read_json(baseline)
    if base.get('version') != 3 or not isinstance(base.get('fields'), dict):
        raise ValueError('A complete Solace version 3 config is required.')
    schema = read_json(HERE.parent / 'analysis' / 'config_trace' / 'rage_config_schema.json')
    plan = read_json(args.plan)
    specs = {x['name']: x for x in schema['group_fields']}
    groups = {x['slug']: x for x in schema['groups']}
    indices = {x['index']: x for x in schema['groups']}
    source_fields = base['fields']
    result = copy.deepcopy(base)
    fields = result['fields']
    names = {}

    def require(category, name, spec):
        key = field_key(category, name)
        if key not in source_fields:
            raise ValueError(f'Baseline lacks {category}.{name} ({key}); save a complete config first.')
        check_value(source_fields[key], spec, f'{category}.{name}')
        return key

    # Check all prospective edits and ancestors before any file writes.
    for group in schema['groups']:
        require(group['category'], 'override general', specs['override general'])
    for slug in plan['weapons']:
        group = groups[slug]
        for name, spec in specs.items():
            require(group['category'], name, spec)
    for item in plan['global']:
        require(item['category'], item['name'], item)
        check_value(item['value'], item, f"{item['category']}.{item['name']}")

    def set_value(category, name, value, spec):
        key = require(category, name, spec)
        check_value(value, spec, f'{category}.{name}')
        if 'ui_range' in spec and not spec['ui_range'][0] <= value <= spec['ui_range'][1]:
            raise ValueError(f'Out of menu range: {category}.{name}')
        fields[key] = copy.deepcopy(value)
        names[key] = f'{category}.{name}'

    for slug, adjustments in plan['weapons'].items():
        group = groups[slug]
        category = group['category']
        override_key = field_key(category, 'override general')
        # Match the menu's add-weapon behavior when enabling an unused group.
        # Read the original baseline, so processing order cannot alter inheritance.
        if slug != 'general' and not source_fields[override_key]['v']:
            parent = indices[group['parent_index']]
            while parent['index'] != 6:
                parent_key = field_key(parent['category'], 'override general')
                if source_fields[parent_key]['v']:
                    break
                parent = indices[parent['parent_index']]
            for name, spec in specs.items():
                if name != 'override general':
                    source_key = require(parent['category'], name, spec)
                    set_value(category, name, source_fields[source_key], spec)
        if slug != 'general':
            set_value(category, 'override general', {'v': True, 'b': None}, specs['override general'])
        for name, value in {**plan['common'], **adjustments}.items():
            set_value(category, name, value, specs[name])
    for item in plan['global']:
        set_value(item['category'], item['name'], item['value'], item)

    changed = {k for k in fields if fields[k] != source_fields[k]}
    if set(fields) != set(source_fields) or not changed <= set(names):
        raise RuntimeError('Unexpected field addition/removal or unplanned modification.')
    for key in base:
        if key != 'fields' and result[key] != base[key]:
            raise RuntimeError(f'Unexpected top-level modification: {key}')
    payload = (json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + '\n').encode('utf-8')
    if json.loads(payload) != result:
        raise RuntimeError('JSON serialization mismatch.')
    report = {
        'status': 'configuration-generated-not-game-loaded',
        'baseline': str(baseline),
        'baseline_sha256': hashlib.sha256(baseline_bytes).hexdigest(),
        'output': str(output),
        'output_sha256': hashlib.sha256(payload).hexdigest(),
        'field_count': len(fields),
        'changes': [{ 'key': k, 'name': names[k], 'before': source_fields[k], 'after': fields[k]}
                    for k in sorted(changed)],
    }
    if not args.dry_run:
        if baseline.read_bytes() != baseline_bytes:
            raise RuntimeError('Baseline changed during generation; repeat after saving finishes.')
        output.parent.mkdir(parents=True, exist_ok=True)
        with report_path.open('x', encoding='utf-8') as handle:
            json.dump(report, handle, ensure_ascii=False, indent=2)
        with output.open('xb') as handle:
            handle.write(payload)
        if output.read_bytes() != payload:
            raise RuntimeError('Output read-back mismatch.')
    print(json.dumps({'dry_run': args.dry_run, 'output': str(output),
                      'field_count': len(fields), 'changed_fields': len(changed)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
