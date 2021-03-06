#! /usr/bin/env python3
from argparse import ArgumentParser
from pathlib import Path
from subprocess import DEVNULL, STDOUT, PIPE
from pprint import pprint
from IPython import embed

import subprocess
import shlex
import re
import pandas as pd

DIR = Path(__file__).absolute().parent

def graph(args):

    df = pd.read_csv(args.input)

    embed()
    exit()

    ax = plt.gca()
    for df in dfs:
        try:
            df = df.drop_duplicates(subset='x')
            pivoted = df.pivot(index='x', columns='label', values='y')
            pivoted.plot.line(ax=ax)
        except:
            embed()
            raise
    plt.xlabel(xlabel)
    plt.ylabel(ylabel)
    plt.legend(loc='best')

    fig = plt.gcf()
    fig.set_size_inches(3.5, 3.0)
    fig.tight_layout()

    plt.savefig(output_file, dpi=300, bbox_inches='tight', pad_inches=0.02)
    plt.close()

def run(args):
    assert 'clean_path' in args
    assert 'fixed_path' in args
    assert args.clean_path.exists(), f'{str(args.clean_path)} does not exist!'
    assert args.fixed_path.exists(), f'{str(args.fixed_path)} does not exist!'

    tp_re = re.compile(r'Throughput: load, (\S+) ,ops/s')

    df_raw = []

    for binary, bin_label in zip([args.clean_path, args.fixed_path], ['original', 'fixed']):
        for threads in args.threads:
            for trial in range(args.ntrials):
                
                subprocess.check_call(shlex.split('rm -rf /mnt/pmem/pool'))

                exp_args = shlex.split(
                    f'numactl -N 1 -m 1 {str(binary)} {args.nkeys} {threads}')

                ret = subprocess.run(exp_args, stdout=PIPE, stderr=STDOUT)
                ret.check_returncode()

                data = {}
                for line in ret.stdout.decode().splitlines():
                    matches = tp_re.match(line)
                    if matches is None:
                        continue

                    throughput = float(matches.group(1))
                    data = {
                        'system': bin_label, 
                        'threads': threads, 
                        'trial_num': trial, 
                        'num_keys': args.nkeys, 
                        'throughput': throughput
                    }
                    break

                if data:
                    df_raw += [data]
                    pprint(data)
                else:
                    print(f'Does not comply: {ret.stdout.decode()}')

    df = pd.DataFrame(df_raw)
    df.to_csv(args.output)


def main():
    parser = ArgumentParser(
        description='Run and graph the RECIPE perf experiment')

    subparser = parser.add_subparsers()
    grapher = subparser.add_parser('graph')
    grapher.set_defaults(fn=graph)
    grapher.add_argument('--input', '-i', type=Path, default='recipe.csv')
    grapher.add_argument('--output', '-o', type=Path, default='recipe.pdf')

    runner = subparser.add_parser('run')
    runner.set_defaults(fn=run)
    runner.add_argument('--output', '-o', type=Path, default='recipe.csv')

    path_fn = lambda p: DIR / p / 'P-CLHT' / 'build' / 'example'
    runner.add_argument('threads', type=int, nargs='+')
    runner.add_argument('--clean-path', '-c', type=Path, default=path_fn('RECIPE-clean'))
    runner.add_argument('--fixed-path', '-f', type=Path, default=path_fn('RECIPE'))
    runner.add_argument('--ntrials', '-t', type=int, default=10)
    runner.add_argument('--nkeys', '-k', type=int, default=10000000)

    args = parser.parse_args()
    assert 'fn' in args, 'Must select a valid subparser command!'
    args.fn(args)


if __name__ == '__main__':
    main()