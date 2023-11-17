#!/usr/bin/python

import os
import re
import csv
from datetime import datetime
import smtplib
from email.mime.text import MIMEText
from subprocess import check_output
import matplotlib.pyplot as plt
import pandas as pd

print('Post commit script is running...')

commit_id=re.search('commit (\w+)', check_output(['git', 'log', '-1', 'HEAD']).decode()).group(1)
print(commit_id)

git_branch = check_output(['git', 'symbolic-ref', '--short', 'HEAD']).strip().decode()
print(git_branch)

#directory = '/home/sumatrad/lsvd-rbd/experiments/results'
directory = '/Users/sumatradhimoyee/Documents/PhDResearch/LSVD/Code/lsvd-rbd/experiments/results'
fio_output_file= os.path.join(directory, 'graphs/fio_output.csv')
print(fio_output_file)
filebench_output_file= os.path.join(directory, 'graphs/filebench_output.csv')

# fio_output= open(fio_output_file, 'w', newline='')
# filebench_output= open(filebench_output_file, 'w', newline='')

# csv_writer_fio=csv.writer(fio_output)
# csv_writer_filebench=csv.writer(filebench_output)

# if fio_output.tell() == 0:
#         csv_writer_fio.writerow(['timestamp', 'commit id', 'git_branch', 'disk_type', 'randread IOPS', 'randwrite IOPS', 'read IOPS', 'write IOPS'])

# if filebench_output.tell() == 0:
#         csv_writer_filebench.writerow(['timestamp', 'commit id', 'git_branch', 'disk_type', 'fileserver IOPS', 'fileserver IOPS/s', 'fileserver-sync IOPS', 'fileserver-sync IOPS/s',  'oltp IOPS', 'oltp IOPS/s', 'varmail IOPS', 'varmail IOPS/s'])

# files = [f for f in os.listdir(directory) if os.path.isfile(os.path.join(directory, f))]
# files.sort(key=lambda x: os.path.getctime(os.path.join(directory, x)), reverse=True)
# recent_files = files[:3]
# for file_name in recent_files:
#     file_path = os.path.join(directory, file_name)
#     #timestamp = os.path.getctime(file_path)
         
#     print(file_path)

#     with open(file_path, 'r') as file:
#         iops_array= [None]*4
#         workload_array= [None]*8
#         file_split= file_name.split('.')
#         timestamp=file_split[0]
#         #print("file_split : ", file_split)
#         if file_split[1]=='rbd':
#             disk_type='rbd'
#         elif file_split[1]=='lsvd':
#             disk_type='lsvd'
#         else:
#             disk_type='ramdisk'
#         for line in file:
#             if line.startswith("RESULT"):
#                 #print(line)
#                 match_fio = re.search("Fio \(iodepth=(\d+)\) (\w+): .* IOPS=([\d.]+[kMG]?)?, BW=([\d.]+(?:[kKMG]i?)?B/s)?", line)
#                 match_filebench = re.search("Filebench /tmp/filebench/(.*?):\d+\.\d+:.* IO Summary: (\d+) ops (\d+\.\d+) ops/s (\d+)/(\d+) rd/wr (\d+\.\d+[kKmMgG]?[Bb]/s)? (\d+\.\d+ms/op)?", line)

#                 #print(match_fio)
#                 #print(match_filebench)
#                 if match_fio:
#                     iodepth_value = match_fio.group(1)
#                     request_type = match_fio.group(2)
#                     iops_value = match_fio.group(3)
#                     bw_value = match_fio.group(4)

#                     #print("request_type: " + request_type)
#                     if iodepth_value=='256':
#                         if request_type=='randread':
#                             iops_array[0]= iops_value
#                         elif request_type=='randwrite':
#                             iops_array[1]= iops_value
#                         elif request_type=='read':
#                             iops_array[2]= iops_value
#                         elif request_type=='write':
#                             iops_array[3]= iops_value


#                     # print("iodepth value: " + iodepth_value)
#                     # print("read value: " + request_type)
#                     # print("IOPS value: " + iops_value)
#                     # print("BW value: " + bw_value)

#                 if match_filebench:
#                     workload_name= match_filebench.group(1)
#                     total_ops = match_filebench.group(2)
#                     ops_per_second = match_filebench.group(3)
#                     read_count, write_count = match_filebench.group(4, 5)
#                     throughput = match_filebench.group(6)
#                     latency = match_filebench.group(7)

#                     if workload_name=='fileserver.f':
#                          workload_array[0]=total_ops
#                          workload_array[1]=ops_per_second
#                     elif workload_name=='fileserver-fsync.f':
#                          workload_array[2]=total_ops
#                          workload_array[3]=ops_per_second
#                     elif workload_name=='oltp.f':
#                          workload_array[4]=total_ops
#                          workload_array[5]=ops_per_second
#                     elif workload_name=='varmail.f':
#                          workload_array[6]=total_ops
#                          workload_array[7]=ops_per_second
                    

#                     # print("workload_name: " + workload_name)
#                     # print("total_ops: " + total_ops)
#                     # print("ops_per_second: " + ops_per_second)
#                     # print("read_count/write_count: " + read_count+"/"+write_count)
#                     # print("throughput: " + throughput)
#                     # print("latency: " + latency)

        
#         csv_writer_fio.writerow([timestamp, commit_id, git_branch, disk_type, iops_array[0], iops_array[1], iops_array[2], iops_array[3]])
#         csv_writer_filebench.writerow([timestamp, commit_id, git_branch, disk_type, workload_array[0], workload_array[1], workload_array[2], workload_array[3], workload_array[4], workload_array[5], workload_array[6], workload_array[7]])

# fio_output.close()
# filebench_output.close()

df_fio = pd.read_csv(fio_output_file)

#unique_lines = df['line'].unique()

fio_col=['randread IOPS', 'randwrite IOPS', 'read IOPS', 'write IOPS']
num_subplots1 = len(fio_col)

fio_group_values = df_fio['disk_type'].unique()

# fig, axes = plt.subplots(num_subplots, 1, figsize=(8, 4 * num_subplots))

num_rows1 = (num_subplots1 + 1) // 2  
fig1, axes1 = plt.subplots(num_rows1, 2, figsize=(12, 4 * num_rows1))

axes1 = axes1.flatten()

#for i, column_name in enumerate(df_fio.columns[1:-1]):
for i, column_name in enumerate(fio_col):  
    for group_value in fio_group_values:
        subset = df_fio[df_fio['disk_type'] == group_value]
        axes1[i].plot(subset['commit id'], subset[column_name], label=f'{group_value}')

    axes1[i].set_xlabel('commit id')
    axes1[i].set_ylabel(f'{column_name}')
    axes1[i].legend()


df_filebench = pd.read_csv(filebench_output_file)


filebench_col=['fileserver IOPS', 'fileserver IOPS/s', 'fileserver-sync IOPS', 'fileserver-sync IOPS/s',  'oltp IOPS', 'oltp IOPS/s', 'varmail IOPS', 'varmail IOPS/s']
num_subplots2 = len(filebench_col)

filebench_group_values = df_filebench['disk_type'].unique()

# fig, axes = plt.subplots(num_subplots, 1, figsize=(8, 4 * num_subplots))

num_rows2 = (num_subplots2 + 1) // 2  
fig2, axes2 = plt.subplots(num_rows2, 2, figsize=(12, 4 * num_rows2))

axes2 = axes2.flatten()

#for i, column_name in enumerate(df_fio.columns[1:-1]):
for i, column_name in enumerate(filebench_col):  
    for group_value in filebench_group_values:
        subset = df_filebench[df_filebench['disk_type'] == group_value]
        axes2[i].plot(subset['commit id'], subset[column_name], label=f'{group_value}')

    axes2[i].set_xlabel('commit id')
    axes2[i].set_ylabel(f'{column_name}')
    axes2[i].legend()




plt.tight_layout()
plt.show()




# for line_value in df['disk_type'].unique():
#     subset = df[df['disk_type'] == line_value]
#     plt.plot(subset['commit id'], subset['randread IOPS'], label=f'Line {line_value}')

# # Add labels and legend
# plt.xlabel('X-axis')
# plt.ylabel('Y-axis')
# plt.legend()

# # Show the plot
# plt.show()

# with open(fio_output_file, 'r') as csv_file:
#     csv_reader = csv.reader(csv_file)
#     header = next(csv_reader)  # Skip header
#     data = list(csv_reader)

#     # Separate iodepth and BW values for plotting
#     iops = [float(re.sub(r'[kMG]?', '', row[4])) for row in data]
#     commit_id = [row[2] for row in data]

#     # Plot the trend
#     plt.plot(commit_id, iops, marker='o')
#     plt.xlabel('commit_id')
#     plt.ylabel('IOPS')
#     plt.title('Trend of IOPS with each commit')
#     plt.grid(True)
#     plt.show()

                

