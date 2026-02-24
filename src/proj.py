import lib.arglib as arglib
import shutil
import sys
import os

gitPath = 'D:/GitHub/'

arglib.usage = 'Usage: proj [mk|rm|mv|ls] [project name]'

arglib.Arg('operation', ('mk','rm','mv','ls'))
arglib.Arg('name')
arglib.Arg('newname')

args = arglib.parse()

if not args:
    print(arglib.usage)
    sys.exit(1)

os.makedirs(gitPath, exist_ok=True)

os.chdir(gitPath)

if args.get('name') and not os.path.exists(args['name']) and args['operation'] != 'mk':
    print(f'Could not find project: {args["name"]}')
    sys.exit(1)

if args['operation'] == 'mk':
    if os.path.exists(args['name']):
        print('Project already exists.')
        sys.exit(1)

    os.makedirs(args['name'], exist_ok=True)
    os.chdir(args['name'])
    os.system('git init -q')
    with open('README.md', 'w') as f:
        f.write(f'\n# {args["name"]}\n\nBlank readme.\n')
    with open('main.py','w') as f:
        f.write('# Created by proj tool\n\n')
    print(f'New project created at: {os.getcwd()}')

elif args['operation'] == 'rm':
    shutil.rmtree(args['name'])
    print(f'Project removed: {args["name"]}')

elif args['operation'] == 'mv':
    if os.path.exists(args['newname']):
        print('Project already exists.')
        sys.exit(1)

    os.rename(args['name'], args['newname'])

elif args['operation'] == 'ls':
    print(',   '.join(os.listdir()))

else:
    print(arglib.usage)
