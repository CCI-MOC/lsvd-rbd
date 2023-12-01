#!/usr/bin/python

import os
import re
import csv
from datetime import datetime
import smtplib
from email.mime.text import MIMEText
import subprocess
from subprocess import check_output
import matplotlib.pyplot as plt
import pandas as pd
import time

## TO USE THIS SCRIPT PLEASE COPY IT UNDER "lsvd-rbd/.git/hook" AND RENAME IT TO "post-commit" ##
## ENSURE THE "directory" VARIABLE IS CHANGED RESPECTIVE TO YOUR REPO ##

print('Post commit script is running...')

commit_id=re.search('commit (\w+)', check_output(['git', 'log', '-1', 'HEAD']).decode()).group(1)[-6:]

git_branch = check_output(['git', 'symbolic-ref', '--short', 'HEAD']).strip().decode()

directory = '/home/sumatrad/lsvd-rbd/experiments/'
# directory = '/Users/sumatradhimoyee/Documents/PhDResearch/LSVD/Code/lsvd-rbd/experiments/'
script_path = os.path.join(directory, 'nightly.bash')
result_dir = os.path.join(directory, 'results')
graph_dir = os.path.join(result_dir, 'graphs')
if not os.path.exists(graph_dir):
    os.makedirs(graph_dir)
fio_output_file= os.path.join(graph_dir, 'fio_output.csv')
fio_plot_file= os.path.join(graph_dir, 'fio_plot')
filebench_output_file= os.path.join(graph_dir, 'filebench_output.csv')
filebench_plot_file= os.path.join(graph_dir, 'filebench_plot')
run_result= os.path.join(directory, datetime.now().strftime("%Y-%m-%d") +'_output.txt')

with open(run_result, 'w') as file:
    os.chdir(directory)
    subprocess.run(['bash', script_path], stdout=file, text=True)

# os.system('./nightly.bash')
# result = subprocess.run(['bash', script_path], stdout=subprocess.PIPE, text=True)
# print(result.stdout)

fio_output= open(fio_output_file, 'a+', newline='')
filebench_output= open(filebench_output_file, 'a+', newline='')

csv_writer_fio=csv.writer(fio_output)
csv_writer_filebench=csv.writer(filebench_output)

if fio_output.tell() == 0:
        csv_writer_fio.writerow(['timestamp', 'commit id', 'git_branch', 'disk_type', 'randread IOPS', 'randwrite IOPS', 'read IOPS', 'write IOPS'])

if filebench_output.tell() == 0:
        csv_writer_filebench.writerow(['timestamp', 'commit id', 'git_branch', 'disk_type', 'fileserver IOPS', 'fileserver IOPS/s', 'fileserver-sync IOPS', 'fileserver-sync IOPS/s',  'oltp IOPS', 'oltp IOPS/s', 'varmail IOPS', 'varmail IOPS/s'])


keywords = ['rbd', 'lsvd', 'ramdisk']
files = [file for file in os.listdir(result_dir) if any(keyword in file for keyword in keywords)]
files.sort(key=lambda x: os.path.getctime(os.path.join(result_dir, x)), reverse=True)
recent_files = files[:3]

for file_name in recent_files:
    file_path = os.path.join(result_dir, file_name)
    #timestamp = os.path.getctime(file_path)
    suffixes = {'k': 1e3, 'm': 1e6, 'g': 1e9}
    iops_array= [None]*4
    workload_array= [None]*8
    file_split= file_name.split('.')
    timestamp=file_split[0]
    if file_split[1]=='rbd':
        disk_type='rbd'
    elif file_split[1]=='lsvd':
        disk_type='lsvd'
    else:
        disk_type='ramdisk'
         
    #print(file_path)

    with open(file_path, 'r') as file:

        for line in file:
            if line.startswith("RESULT"):
                #print(line)
                match_fio = re.search("Fio \(iodepth=(\d+)\) (\w+): .* IOPS=([\d.]+[kKmMgG]?)?, BW=([\d.]+(?:[kKmMgG]i?)?B/s)?", line)
                match_filebench = re.search("Filebench /tmp/filebench/(.*?):\d+\.\d+:.* IO Summary: (\d+) ops (\d+\.\d+) ops/s (\d+)/(\d+) rd/wr (\d+\.\d+[kKmMgG]?[Bb]/s)? (\d+\.\d+ms/op)?", line)

                # print(match_fio)
                # print(match_filebench)
                if match_fio:
                    iodepth_value = match_fio.group(1)
                    request_type = match_fio.group(2)
                    iops_value = match_fio.group(3)
                    
                    suffix = iops_value[-1].lower()

                    if suffix in suffixes:
                        iops_value = float(iops_value[:-1]) * suffixes[suffix]
                    else:
                        iops_value = float(iops_value)
                    bw_value = match_fio.group(4)

                    
                    if iodepth_value=='256':
                        if request_type=='randread':
                            iops_array[0]= iops_value
                        elif request_type=='randwrite':
                            iops_array[1]= iops_value
                        elif request_type=='read':
                            iops_array[2]= iops_value
                        elif request_type=='write':
                            iops_array[3]= iops_value


                if match_filebench:
                    workload_name= match_filebench.group(1)
                    total_ops = match_filebench.group(2)
                    ops_per_second = match_filebench.group(3)
                    read_count, write_count = match_filebench.group(4, 5)
                    throughput = match_filebench.group(6)
                    latency = match_filebench.group(7)

                    if workload_name=='fileserver.f':
                         workload_array[0]=total_ops
                         workload_array[1]=ops_per_second
                    elif workload_name=='fileserver-fsync.f':
                         workload_array[2]=total_ops
                         workload_array[3]=ops_per_second
                    elif workload_name=='oltp.f':
                         workload_array[4]=total_ops
                         workload_array[5]=ops_per_second
                    elif workload_name=='varmail.f':
                         workload_array[6]=total_ops
                         workload_array[7]=ops_per_second

        
        csv_writer_fio.writerow([timestamp, commit_id, git_branch, disk_type, iops_array[0], iops_array[1], iops_array[2], iops_array[3]])
        csv_writer_filebench.writerow([timestamp, commit_id, git_branch, disk_type, workload_array[0], workload_array[1], workload_array[2], workload_array[3], workload_array[4], workload_array[5], workload_array[6], workload_array[7]])

fio_output.close()
filebench_output.close()

df_fio = pd.read_csv(fio_output_file)


fio_col=['randread IOPS', 'randwrite IOPS', 'read IOPS', 'write IOPS']
num_subplots1 = len(fio_col)

fio_group_values = df_fio['disk_type'].unique()

# fig1, axes1 = plt.subplots(num_subplots1, 1, figsize=(8, 4 * num_subplots1))

num_rows1 = (num_subplots1 + 1) // 2  
fig1, axes1 = plt.subplots(num_rows1, 2, figsize=(12, 4 * num_rows1))

axes1 = axes1.flatten()

#for i, column_name in enumerate(df_fio.columns[1:-1]):
for i, column_name in enumerate(fio_col):  
    for group_value in fio_group_values:
        subset = df_fio[df_fio['disk_type'].values == group_value]
        axes1[i].plot(subset['commit id'].values, subset[column_name].values, label=f'{group_value}')

    axes1[i].set_xlabel('commit id')
    axes1[i].set_ylabel(f'{column_name}')
    axes1[i].legend()

plt.suptitle('Fio Results for LSVD, RBD and Ramdisk', y=1.02, fontsize=16)
# axes1[i].title('Fio Results for LSVD, RBD and Ramdisk')


df_filebench = pd.read_csv(filebench_output_file)


filebench_col=['fileserver IOPS', 'fileserver IOPS/s', 'fileserver-sync IOPS', 'fileserver-sync IOPS/s',  'oltp IOPS', 'oltp IOPS/s', 'varmail IOPS', 'varmail IOPS/s']
num_subplots2 = len(filebench_col)

filebench_group_values = df_filebench['disk_type'].unique()

# fig2, axes2 = plt.subplots(num_subplots2, 1, figsize=(8, 4 * num_subplots2))

num_rows2 = (num_subplots2 + 1) // 2  
fig2, axes2 = plt.subplots(num_rows2, 2, figsize=(12, 4 * num_rows2))

axes2 = axes2.flatten()

#for i, column_name in enumerate(df_fio.columns[1:-1]):
for i, column_name in enumerate(filebench_col):  
    for group_value in filebench_group_values:
        subset = df_filebench[df_filebench['disk_type'].values == group_value]
        axes2[i].plot(subset['commit id'].values, subset[column_name].values, label=f'{group_value}')

    axes2[i].set_xlabel('commit id')
    axes2[i].set_ylabel(f'{column_name}')
    axes2[i].legend()


plt.suptitle('Filebench Results for LSVD, RBD and Ramdisk', y=.06, fontsize=20)

fig1.savefig(fio_plot_file, bbox_inches='tight')
fig2.savefig(filebench_plot_file, bbox_inches='tight')

plt.tight_layout()
#plt.show()

print("Post-commit script ended")



                


