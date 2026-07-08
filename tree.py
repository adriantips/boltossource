import os

def print_tree(dir_path, prefix=''):
    contents = sorted([c for c in os.listdir(dir_path) if c not in ['.git', 'build', '__pycache__', 'tree.py'] and not c.endswith('.log')])
    pointers = ['+-- '] * (len(contents) - 1) + ['\\-- '] if contents else []
    for pointer, name in zip(pointers, contents):
        path = os.path.join(dir_path, name)
        print(prefix + pointer + name)
        if os.path.isdir(path):
            extension = '|   ' if pointer == '+-- ' else '    '
            print_tree(path, prefix + extension)

print_tree('.')
