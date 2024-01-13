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
import hashlib
import numpy as np


curr_dt = datetime.now().strftime("%m-%d_%H:%M")
commit_id=re.search('commit (\w+)', check_output(['git', 'log', '-1', 'HEAD']).decode()).group(1)[-6:]
# hash_id=curr_dt + "_" + commit_id
# print("hash_id", hash_id)

git_branch = check_output(['git', 'symbolic-ref', '--short', 'HEAD']).strip().decode()



# directory = '/home/sumatrad/lsvd-rbd/experiments/'
directory = '/Users/sumatradhimoyee/Documents/PhDResearch/LSVD/lsvd-rbd/experiments/'
script_path = os.path.join(directory, 'nightly.bash')
result_dir = os.path.join(directory, 'results/jan_11_res/')
print("result_dir: " + result_dir)
graph_dir = os.path.join(result_dir, 'graphs')
if not os.path.exists(graph_dir):
    os.makedirs(graph_dir)
fio_output_file= os.path.join(graph_dir, 'fio_output.csv')
fio_plot_file= os.path.join(graph_dir, 'fio_plot_1')
fio_output_file_2= os.path.join(graph_dir, 'fio_output_all.csv')
fio_plot_file_2= os.path.join(graph_dir, 'fio_plot_2')
filebench_output_file= os.path.join(graph_dir, 'filebench_output.csv')
filebench_plot_file= os.path.join(graph_dir, 'filebench_plot_1')
#run_result= os.path.join(directory, curr_dt +'_output_1.txt')

start_time = time.time()

# with open(run_result, 'w') as file:
#     os.chdir(directory)
#     subprocess.run(['bash', script_path], stdout=file, text=True)

end_time = time.time()


fio_output= open(fio_output_file, 'w', newline='')
fio_output_2= open(fio_output_file_2, 'w', newline='')
filebench_output= open(filebench_output_file, 'w', newline='')

csv_writer_fio=csv.writer(fio_output)
csv_writer_fio_2=csv.writer(fio_output_2)
csv_writer_filebench=csv.writer(filebench_output)

if fio_output.tell() == 0:
        csv_writer_fio.writerow(['timestamp', 'hash_id', 'disk_type', 'pool_type', 'cache_size', 'randread_IOPS', 'randread_BW' 'randwrite_IOPS', 'randwrite_BW', 'read_IOPS', 'read_BW', 'write_IOPS', 'write_BW'])

if fio_output_2.tell() == 0:
        csv_writer_fio_2.writerow(['timestamp', 'hash_id', 'disk_type', 'pool_type', 'cache_size', 'workload', 'iodepth', 'blocksize', 'IOPS', 'ext_iops', 'throughput', 'bw'])

if filebench_output.tell() == 0:
        csv_writer_filebench.writerow(['timestamp', 'hash_id', 'disk_type', 'pool_type', 'cache_size', 'fileserver_IOPS', 'fileserver_throughput', 'fileserver-sync_IOPS', 'fileserver-sync_throughput',  'oltp_IOPS', 'oltp_throughput', 'varmail_IOPS', 'varmail_throughput'])

n_rbd=0
n_lvsd=0
n_ram=0
n_nvme=0

keywords = ['rbd', 'lsvd']
files = [file for file in os.listdir(result_dir) if any(keyword in file for keyword in keywords)]
files.sort(key=lambda x: os.path.getctime(os.path.join(result_dir, x)))
#recent_files = files[:3]

for file_name in files:
    file_path = os.path.join(result_dir, file_name)
    print(file_name)
    #timestamp = os.path.getctime(file_path)
    suffixes = {'k': 1e3, 'm': 1e6, 'g': 1e9}
    suffixes_bw = {'kib/s': 1e3, 'mib/s': 1e6, 'gib/s': 1e9}
    iops_array= [None]*4
    bw_array= [None]*4
    workload_array= [None]*8
    file_split= file_name.split('.')
    timestamp=file_split[0]
    #cache_rex=file_split[0]
    edate=file_split[0].split('T')
    hash_id=str(edate[0][2]+edate[0][3]+edate[0][5]+edate[0][6]+edate[0][8]+edate[0][9])
    #print("hash_id: "+hash_id)


    if file_split[1]=='rbd':
        disk_type='rbd'
        cache_size='none'
        n_rbd=n_rbd+1
    elif file_split[1]=='lsvd-240':
        disk_type='lsvd'
        cache_size='240gb'
        n_lvsd=n_lvsd+1
    elif file_split[1]=='lsvd-20':
        disk_type='lsvd'
        cache_size='20gb'
        n_lvsd=n_lvsd+1
    elif file_split[1]=='nvme':
        disk_type='nvme'
        cache_size='none'
        pool_type='none'
        n_nvme=n_nvme+1
    else:
        disk_type='ramdisk'
        cache_size='none'
        pool_type='none'
        n_ram=n_ram+1
        #print("else: " +file_name)

    if file_split[2]=='rssd2':
        pool_type='ssd'
    elif file_split[2]=='triple-hdd':
        pool_type='hdd'

    with open(file_path, 'r') as file:
        workload = ""
        time_value = 0
        iodepth = ""
        bs = ""
        iops=""
        iops_value = 0
        bw= 0
        bw_value=0
        fio=False
        result=False
        print("filename: "+file_name)
        print("cache_size: "+cache_size)
        for line in file:
            if line.startswith("===Fio: "):
                match = re.search("===Fio: workload=(\w+), time=(\d+), iodepth=(\d+), bs=(\w+)===", line)
                #match = re.search("===Fio: workload=(\w+), time=(\d+), iodepth=(\w+), bs=([\w\d]+)", line)
                #print(match)
                if match:
                    fio=True
                    workload = match.group(1)
                    time_value = int(match.group(2))
                    iodepth = int(match.group(3))
                    bs = match.group(4)


                    print("Workload:", workload)
                    # print("Time:", time_value)
                    # print("Iodepth:", iodepth)
                    # print("Block Size:", bs)
                # else:
                #     print("Pattern not found in the line.")

            if line.startswith("RESULT"):
                #print(line)
                #match_fio = re.search("Fio \(iodepth=(\d+)\) (\w+): .* IOPS=([\d.]+[kKmMgG]?)?, BW=([\d.]+(?:[kKmMgG]i?)?B/s)?", line)
                match_fio = re.search("Fio \(iodepth=(\d+); bs=(\w+)\) (\w+): .* IOPS=([\d.]+[kKmMgG]?)?, BW=([\d.]+(?:[kKmMgG]i?)?B/s)?", line)
                #match_fio = re.search("Fio \(iodepth=(\d+)\) (\w+): .* IOPS=([\d.]+[kKmMgG]?)?, BW=([\d.]+)([BKMGTPEZY]?B/s)?", line)
                #match_filebench = re.search("Filebench /tmp/filebench/(.*?):\d+\.\d+:.* IO Summary: (\d+) ops (\d+\.\d+) ops/s (\d+)/(\d+) rd/wr (\d+\.\d+[kKmMgG]?[Bb]/s)? (\d+\.\d+ms/op)?", line)
                #match_filebench = re.search("Filebench /tmp/filebench/(.*?):\d+\.\d+:.* IO Summary: (\d+) ops (\d+\.\d+) .* (\d+\.\d+[kKmMgG]?[Bb]/s)? (\d+\.\d+ms/op)?", line)
                match_filebench = re.search("Filebench /tmp/filebench/(.*?):\d+\.\d+:.* IO Summary: (\d+) ops (\d+\.\d+) .* (\d+\.\d+)[kKmMgG]?[Bb]/s? (\d+\.\d+ms/op)?", line)

                print(match_fio)
                #print(match_filebench)
                if match_fio:
                    result=True
                    iodepth_value = match_fio.group(1)
                    bs_value= match_fio.group(2)
                    request_type = match_fio.group(3)
                    iops = match_fio.group(4)
                    bw= match_fio.group(5)
                    # print("iops_value: "+ iops_value)
                    # print("bw: "+ bw)
                    # bw2= match_fio.group(5)
                    # print("bw1: "+ bw2)
                    
                    suffix = iops[-1].lower()

                    if suffix in suffixes:
                        iops_value = float(iops[:-1]) * suffixes[suffix]/1000
                    else:
                        iops_value = float(iops)/1000


                    
                    suffix_bw = bw[-5:].lower()
                    print(suffix_bw)

                    if suffix_bw in suffixes_bw:
                        bw_value = float(bw[:-5]) * suffixes_bw[suffix_bw]/1000000
                    else:
                        bw_value = float(bw)/1000
                    #print('bw_value: ' + str(bw_value))
                    #bw_value = match_fio.group(4)

                    
                    if iodepth_value=='256':
                        if request_type=='randread':
                            iops_array[0]= iops_value
                            bw_array[0]= bw
                        elif request_type=='randwrite':
                            iops_array[1]= iops_value
                            bw_array[1]= bw
                        elif request_type=='read':
                            iops_array[2]= iops_value
                            bw_array[2]= bw
                        elif request_type=='write':
                            iops_array[3]= iops_value
                            bw_array[3]= bw


                if match_filebench:
                    workload_name= match_filebench.group(1)
                    # print(file_name)
                    # print(workload_name)
                    total_ops = match_filebench.group(2)
                    ops_per_second = match_filebench.group(3)
                    #read_count, write_count = match_filebench.group(4, 5)
                    #print("read: " + read_count)
                    throughput = match_filebench.group(4)
                    thr_per_ops= match_filebench.group(5)
                    # print(file_name)
                    # print(workload_name+" : " + throughput)
                    # print("thr_per_ops : " + thr_per_ops)
                    #latency = match_filebench.group(7)

                    if workload_name=='fileserver.f':
                         workload_array[0]=total_ops
                         workload_array[1]=throughput
                        #  print("Inside File")
                    elif workload_name=='fileserver-fsync.f':
                         workload_array[2]=total_ops
                         workload_array[3]=throughput
                    elif workload_name=='oltp.f':
                         workload_array[4]=total_ops
                         workload_array[5]=throughput
                    elif workload_name=='varmail.f':
                         workload_array[6]=total_ops
                         workload_array[7]=throughput
            if(fio and result):
                print("INSIDE: " + workload)
                csv_writer_fio_2.writerow([timestamp, hash_id, disk_type, pool_type, cache_size, workload, iodepth, bs, iops, iops_value, bw, bw_value])
                fio=False
                result=False
        
        
        csv_writer_fio.writerow([timestamp, hash_id, disk_type, iops_array[0], bw_array[0], iops_array[1], bw_array[1], iops_array[2], bw_array[2], iops_array[3], bw_array[3]])
        csv_writer_filebench.writerow([timestamp, hash_id, disk_type, pool_type, cache_size, workload_array[0], workload_array[1], workload_array[2], workload_array[3], workload_array[4], workload_array[5], workload_array[6], workload_array[7]])

fio_output.close()
fio_output_2.close()
filebench_output.close()