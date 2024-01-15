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

directory = '/Users/sumatradhimoyee/Documents/PhDResearch/LSVD/lsvd-rbd/experiments/results/'
result_folder= 'fusion_results'
result_dir = os.path.join(directory, result_folder)
graph_dir = os.path.join(result_dir, 'graphs')
if not os.path.exists(graph_dir):
    os.makedirs(graph_dir)
multi_fio_csv= os.path.join(graph_dir, 'multi_fio.csv')
multi_gw_fio_csv= os.path.join(graph_dir, 'multi_gw_fio.csv')
single_fio_csv= os.path.join(graph_dir, 'single_fio.csv')
single_filebench_csv= os.path.join(graph_dir, 'single_filebench.csv')
fileserver_ops_csv= os.path.join(graph_dir, 'fileserver_ops.csv')
oltp_ops_csv= os.path.join(graph_dir, 'oltp_ops.csv')
randomwrite_ops_csv= os.path.join(graph_dir, 'randomwrite_ops.csv')
varmail_ops_csv= os.path.join(graph_dir, 'varmail_ops.csv')


multi_fio_file= open(multi_fio_csv, 'w', newline='')
multi_gw_fio_file= open(multi_gw_fio_csv, 'w', newline='')
single_fio_file= open(single_fio_csv, 'w', newline='')
single_filebench_file= open(single_filebench_csv, 'w', newline='')
fileserver_ops_file= open(fileserver_ops_csv, 'w', newline='')
oltp_ops_file= open(oltp_ops_csv, 'w', newline='')
randomwrite_ops_file= open(randomwrite_ops_csv, 'w', newline='')
varmail_ops_file= open(varmail_ops_csv, 'w', newline='')

multi_fio_csv_writer=csv.writer(multi_fio_file)
multi_gw_fio_csv_writer=csv.writer(multi_gw_fio_file)
single_fio_csv_writer=csv.writer(single_fio_file)
single_filebench_csv_writer=csv.writer(single_filebench_file)
fileserver_ops_csv_writer=csv.writer(fileserver_ops_file)
oltp_ops_csv_writer=csv.writer(oltp_ops_file)
randomwrite_ops_csv_writer=csv.writer(randomwrite_ops_file)
varmail_ops_csv_writer=csv.writer(varmail_ops_file)

if multi_fio_file.tell() == 0:
        multi_fio_csv_writer.writerow(['disk_type', 'pool_type', 'cache_size', 'iops_min', 'iops_max', 'iops_avg', 'iops_std', 'iops_samples', 'bw_min', 'bw_max', 'bw_perc', 'bw_avg', 'bw_std', 'bw_samples', 'clat_min', 'clat_max', 'clat_avg', 'clat_std', 'iodepth', 'bs', 'disks', 'workload', 'request_type', 'iops', 'bw', 'bw_mb', 'total_bw', 'total_time', 'clat_1th', 'clat_50th', 'clat_90th', 'clat_99th'])

if multi_gw_fio_file.tell() == 0:
        multi_gw_fio_csv_writer.writerow(['disk_type', 'pool_type', 'cache_size', 'iops_min', 'iops_max', 'iops_avg', 'iops_std', 'iops_samples', 'bw_min', 'bw_max', 'bw_perc', 'bw_avg', 'bw_std', 'bw_samples', 'clat_min', 'clat_max', 'clat_avg', 'clat_std', 'iodepth', 'bs', 'gateways', 'workload', 'request_type', 'iops', 'bw', 'bw_mb', 'total_bw', 'total_time', 'clat_1th', 'clat_50th', 'clat_90th', 'clat_99th'])

if single_fio_file.tell() == 0:
        single_fio_csv_writer.writerow(['disk_type', 'pool_type', 'cache_size', 'iops_min', 'iops_max', 'iops_avg', 'iops_std', 'iops_samples', 'bw_min', 'bw_max', 'bw_perc', 'bw_avg', 'bw_std', 'bw_samples', 'clat_min', 'clat_max', 'clat_avg', 'clat_std', 'iodepth', 'bs', 'iops_limit', 'workload', 'request_type', 'iops', 'bw', 'bw_mb', 'total_bw', 'total_time', 'clat_1th', 'clat_50th', 'clat_90th', 'clat_99th'])

if single_filebench_file.tell() == 0:
        single_filebench_csv_writer.writerow(['disk_type', 'pool_type', 'cache_size', 'file_type', 'file_time', 'file_total_ops' , 'file_ops_per_second' , 'file_read' , 'file_write', 'file_bw' , 'file_bw_per_ops'])

if fileserver_ops_file.tell() == 0:
        fileserver_ops_csv_writer.writerow(['disk_type', 'pool_type', 'cache_size', 'file_type', 'operation_type', 'operation_iops', 'operation_iops_per_sec', 'operation_bw', 'operation_bw_per_op'])

if oltp_ops_file.tell() == 0:
        oltp_ops_csv_writer.writerow(['disk_type', 'pool_type', 'cache_size', 'file_type', 'operation_type', 'operation_iops', 'operation_iops_per_sec', 'operation_bw', 'operation_bw_per_op'])

if randomwrite_ops_file.tell() == 0:
        randomwrite_ops_csv_writer.writerow(['disk_type', 'pool_type', 'cache_size', 'file_type', 'operation_type', 'operation_iops', 'operation_iops_per_sec', 'operation_bw', 'operation_bw_per_op'])

if varmail_ops_file.tell() == 0:
        varmail_ops_csv_writer.writerow(['disk_type', 'pool_type', 'cache_size', 'file_type', 'operation_type', 'operation_iops', 'operation_iops_per_sec', 'operation_bw', 'operation_bw_per_op'])




keywords = ['rbd', 'lsvd', 'nvme']
files = [file for file in os.listdir(result_dir) if any(keyword in file for keyword in keywords)]
files.sort(key=lambda x: os.path.getctime(os.path.join(result_dir, x)))
for file_name in files:
    file_path = os.path.join(result_dir, file_name)
    print(file_name)
    file_split= file_name.split('.')


    if file_split[1]=='rbd':
        disk_type='rbd'
        cache_size='none'
        # pool_type=file_split[2]
        if file_split[2]=='rssd2':
            pool_type='ssd'
        elif file_split[2]=='triple-hdd':
            pool_type='hdd'
    elif file_split[1]=='lsvd-240':
        disk_type='lsvd'
        cache_size='240gb'
        # pool_type=file_split[2]
        if file_split[2]=='rssd2':
            pool_type='ssd'
        elif file_split[2]=='triple-hdd':
            pool_type='hdd'
    elif file_split[1]=='lsvd-20':
        disk_type='lsvd'
        cache_size='20gb'
        # pool_type=file_split[2]
        if file_split[2]=='rssd2':
            pool_type='ssd'
        elif file_split[2]=='triple-hdd':
            pool_type='hdd'
    elif file_split[1]=='lsvd-multi':
        disk_type='lsvd-multi'
        cache_size='none'
        # pool_type=file_split[2]
        if file_split[2]=='rssd2':
            pool_type='ssd'
        elif file_split[2]=='triple-hdd':
            pool_type='hdd'
    elif file_split[1]=='rbd-multi':
        disk_type='rbd-multi'
        cache_size='none'
        # pool_type=file_split[2]
        if file_split[2]=='rssd2':
            pool_type='ssd'
        elif file_split[2]=='triple-hdd':
            pool_type='hdd'
    elif file_split[1]=='nvme-multi':
        disk_type='nvme-multi'
        cache_size='none'
        pool_type='none'
    elif file_split[1]=='lsvd-multigw':
        disk_type='lsvd-multigw'
        cache_size='none'
        # pool_type=file_split[2]
        if file_split[2]=='rssd2':
            pool_type='ssd'
        elif file_split[2]=='triple-hdd':
            pool_type='hdd'
    elif file_split[1]=='rbd-multigw':
        disk_type='lsvd-multigw'
        cache_size='none'
        # pool_type=file_split[2]
        if file_split[2]=='rssd2':
            pool_type='ssd'
        elif file_split[2]=='triple-hdd':
            pool_type='hdd'
    elif file_split[1]=='lsvd-malloc':
        disk_type='lsvd-malloc'
        if(file_split[2]=='20'):
            cache_size='20gb'  
        elif file_split[2]=='240':
            cache_size='240gb' 
        else:
            cache_size='none'

        if file_split[3]=='rssd2':
            pool_type='ssd'
        elif file_split[3]=='triple-hdd':
            pool_type='hdd'
        else:
            cache_size='none'
    elif file_split[1]=='lsvd-nvme':
        disk_type='lsvd-nvme'
        if(file_split[2]=='20'):
            cache_size='20gb'  
        elif file_split[2]=='240':
            cache_size='240gb' 
        else:
            cache_size='none'

        if file_split[3]=='rssd2':
            pool_type='ssd'
        elif file_split[3]=='triple-hdd':
            pool_type='hdd'

    elif file_split[1]=='nvme':
        disk_type='nvme'
        cache_size='none'
        pool_type='none'
    else:
        disk_type=file_split[1]
        cache_size='none'
        pool_type='none'
        print("else: " +file_name)

    with open(file_path, 'r') as file:
        iops_min = ""
        iops_max = ""
        iops_avg = ""
        iops_std = ""
        iops_samples = ""

        bw_min = ""
        bw_max = ""
        bw_perc = ""
        bw_avg = ""
        bw_std = ""
        bw_samples = ""

        clat_min = ""
        clat_max = ""
        clat_avg = ""
        clat_std = ""

        iodepth = ""
        bs = ""
        iops_limit = ""
        disks = ""
        gateways = ""
        workload= ""
        request_type= ""
        iops = ""
        bw = ""
        bw_mb = ""
        total_bw = ""
        total_time = ""

        clat_1th = ""
        clat_50th = ""
        clat_90th = ""
        clat_99th = ""

        file_type= "fileserver"
        file_time= ""
        file_total_ops = ""
        file_ops_per_second = ""
        file_read = ""
        file_write= ""
        file_bw = ""
        file_bw_per_ops = ""
        operation_type= ""
        operation_iops= ""
        operation_iops_per_sec= ""
        operation_bw= ""
        operation_bw_per_op= ""
        fio=False
        result=False
        ops_bool=False
        file_bool=False
        for line in file:

            pattern_sum=r"RESULT:\s*Fio\s*\(iodepth=(\d+);\s*bs=(\d+ki),\s*iops_limit=(\d+)\)\s*(\w+):\s*\s*(\w+):\s*IOPS=(\S+),\s*BW=(\S+)\s*\((\S+)\)\((\S+)/(\S+)\)"
            pattern_sum2=r"RESULT:\s*Fio\s*\(iodepth=(\d+);\s*bs=(\d+ki)\)\s*(\w+):\s*\s*(\w+):\s*IOPS=(\S+),\s*BW=(\S+)\s*\((\S+)\)\((\S+)/(\S+)\)"
            pattern_clat=r"clat\s*\(usec\):\s*min=(\d+),\s*max=(\d+),\s*avg=([\d.]+),\s*stdev=([\d.]+)"
            pattern_bw=r"bw\s*\(\s*\s*KiB/s\):\s*min=(\d+),\s*max=(\d+),\s*per=([\d.]+)%,\s*avg=([\d.]+),\s*stdev=([\d.]+),\s*samples=(\d+)"
            #pattern_iops=r"iops\s*:\s*min=(\d+),\s*max=(\d+),\s*avg=([\d.]+),\s*stdev=([\d.]+),\s*samples=(\d+)"
            pattern_iops=r"iops\s*:\s*min=\s*(\d+),\s*max=\s*(\d+),\s*avg=\s*([\d.]+),\s*stdev=\s*([\d.]+),\s*samples=\s*(\d+)"
            #iops        : min= 4060, max= 5886, avg=5330.73, stdev=345.41, samples=60
            pattern_clat_perc=r"(\d+\.\d{2}th)=\[\s*(\d+)\]"
            #multi_pattern_sum=r"RESULT:\s*Fio\s*\(disks=(\d+),\s*iodepth=(\d+);\s*bs=([\w]+)\)\s*([\w]+):\s*([\w]+):\s*IOPS=(\S+),\s*BW=([\d\.]+[KMGTPEZY]?i?B/s)\s*\((\S+)\)\((\S+)/(\S+)\)"
            multi_pattern_sum=r"RESULT:\s*Fio\s*\(disks=(\d+),\s*iodepth=(\d+);\s*bs=([\w]+)\)\s*([\w]+):\s*([\w]+):\s*IOPS=(\S+),\s*BW=(\S+)\s*\((\S+)\)\((\S+)/(\S+)\)"
            #RESULT: Fio (disks=4, iodepth=256; bs=4ki) randread:  read: IOPS=173k, BW=674MiB/s (707MB/s)(118GiB/180001msec)
            multi_pattern_clat=r"clat\s*\(usec\):\s*min=(\d+),\s*max=(\d+),\s*avg=([\d.]+),\s*stdev=([\d.]+)"
            multi_pattern_bw=r"bw\s*\(\s*KiB/s\):\s*min=(\d+),\s*max=(\d+),\s*per=([\d.]+)%,\s*avg=([\d.]+),\s*stdev=([\d.]+),\s*samples=(\d+)"
            multi_pattern_iops=r"iops\s*:\s*min=(\d+),\s*max=(\d+),\s*avg=([\d.]+),\s*stdev=([\d.]+),\s*samples=(\d+)"
            #multi_pattern_iops=r"iops\s+:\s+min=(\d+),\s+max=(\d+),\s+avg=([\d.]+),\s+stdev=([\d.]+),\s+samples=(\d+)"
            multi_pattern_clat_perc=r"(\d+\.\d{2}th)=\[\s*(\d+)\]"
            multi_gw_pattern_sum=r"RESULT:\s*Fio\s*\(gateways=(\d+),\s*iodepth=(\d+);\s*bs=([\w]+)\)\s*([\w]+):\s*([\w]+):\s*IOPS=(\S+),\s*BW=(\S+)\s*\((\S+)\)\((\S+)/(\S+)\)"
            multi_gw_pattern_clat=r"clat\s*\(usec\):\s*min=(\d+),\s*max=(\d+),\s*avg=([\d.]+),\s*stdev=([\d.]+)"
            multi_gw_pattern_bw=r"bw\s*\(\s*KiB/s\):\s*min=(\d+),\s*max=(\d+),\s*per=([\d.]+)%,\s*avg=([\d.]+),\s*stdev=([\d.]+),\s*samples=(\d+)"
            multi_gw_pattern_iops=r"iops\s*:\s*min=(\d+),\s*max=(\d+),\s*avg=([\d.]+),\s*stdev=([\d.]+),\s*samples=(\d+)"
            multi_gw_pattern_clat_perc=r"(\d+\.\d{2}th)=\[\s*(\d+)\]"

            # multi_match_clat_perc = re.search(multi_pattern_clat_perc, line)
            # if multi_match_clat_perc:
            #     #print(multi_match_clat_perc)
            #     clat_perc = multi_match_clat_perc.group(1)
            #     clat_perc_val = multi_match_clat_perc.group(2)
            #     print(clat_perc, ":", clat_perc_val)  

            multi_match_clat_perc = re.finditer(multi_pattern_clat_perc, line)
            if multi_match_clat_perc:
                fio=True
                #print("Values extracted:")
                for match in multi_match_clat_perc:
                    #print(f"{match.group(1)}th: {match.group(2)}")
                    if match.group(1)=="1.00th":
                        clat_1th= match.group(2)
                    elif match.group(1)=="50.00th":
                        clat_50th= match.group(2)
                    elif match.group(1)=="90.00th":
                        clat_90th= match.group(2)
                    elif match.group(1)=="99.99th":
                        clat_99th= match.group(2)

            multi_match_iops = re.search(multi_pattern_iops, line)
            if multi_match_iops:
                fio=True
                iops_min = multi_match_iops.group(1)
                iops_max = multi_match_iops.group(2)
                iops_avg = multi_match_iops.group(3)
                iops_std = multi_match_iops.group(4)
                iops_samples = multi_match_iops.group(5)
            
                # print("iops_Min:", iops_min)
                # print("iops_Max:", iops_max)
                # print("iops_Avg:", iops_avg)
                # print("iops_Stdev:", iops_std)
                # print("iops_Samples:", iops_samples)


            multi_match_bw = re.search(multi_pattern_bw, line)
            if multi_match_bw:
                fio=True
                #print(multi_match_bw)
                bw_min = multi_match_bw.group(1)
                bw_max = multi_match_bw.group(2)
                bw_perc = multi_match_bw.group(3)
                bw_avg = multi_match_bw.group(4)
                bw_std = multi_match_bw.group(5)
                bw_samples = multi_match_bw.group(6)
            
                # print("bw_Min:", bw_min)
                # print("bw_Max:", bw_max)
                # print("bw_Per:", bw_perc)
                # print("bw_Avg:", bw_avg)
                # print("bw_Stdev:", bw_std)
                # print("bw_Samples:", bw_samples)
            

            multi_match_clat = re.search(multi_pattern_clat, line)  
            if multi_match_clat:
                fio=True
                #print(multi_match_clat)
                clat_min = multi_match_clat.group(1)
                clat_max = multi_match_clat.group(2)
                clat_avg = multi_match_clat.group(3)
                clat_std = multi_match_clat.group(4)
            
                # print("clat_Min:", clat_min)
                # print("clat_Max:", clat_max)
                # print("clat_Average:", clat_avg)
                # print("clat_Standard_Deviation:", clat_std)


            multi_match_sum = re.search(multi_pattern_sum, line)
            if multi_match_sum:
                result=True
                #print(multi_match_sum) 
                disks = multi_match_sum.group(1)
                iodepth = multi_match_sum.group(2)
                bs = multi_match_sum.group(3)
                workload= multi_match_sum.group(4)
                request_type= multi_match_sum.group(5)
                iops = multi_match_sum.group(6)
                
                bw = multi_match_sum.group(7)
                bw_mb = multi_match_sum.group(8)
                total_bw = multi_match_sum.group(9)
                total_time = multi_match_sum.group(10)
            
                # print("Disks:", disks)
                # print("Iodepth:", iodepth)
                # print("Block Size:", bs)
                # print("Workload:", workload)
                # print("Operations:", request_type)
                # print("IOPS:", iops)
                # print("Bandwidth:", bw)
                # print("Bandwidth Converted:", bw_mb)
                # print("Total Size:", total_bw)
                # print("Total Time:", total_time)
                
                # print("IOPS:", iops)
                # print("Bandwidth:", bw)
                # print("Total Bandwidth:", bw_mb)
                # print("Total Size:", total_bw)



            multi_gw_match_clat_perc = re.finditer(multi_gw_pattern_clat_perc, line)
            if multi_gw_match_clat_perc:
                fio=True
                #print("Values extracted:")
                for match in multi_gw_match_clat_perc:
                    #print(f"{match.group(1)}th: {match.group(2)}")
                    if match.group(1)=="1.00th":
                        clat_1th= match.group(2)
                    elif match.group(1)=="50.00th":
                        clat_50th= match.group(2)
                    elif match.group(1)=="90.00th":
                        clat_90th= match.group(2)
                    elif match.group(1)=="99.99th":
                        clat_99th= match.group(2)

            multi_gw_match_iops = re.search(multi_gw_pattern_iops, line)
            if multi_gw_match_iops:
                fio=True
                iops_min = multi_gw_match_iops.group(1)
                iops_max = multi_gw_match_iops.group(2)
                iops_avg = multi_gw_match_iops.group(3)
                iops_std = multi_gw_match_iops.group(4)
                iops_samples = multi_gw_match_iops.group(5)
            
                # print("iops_Min:", iops_min)
                # print("iops_Max:", iops_max)
                # print("iops_Avg:", iops_avg)
                # print("iops_Stdev:", iops_std)
                # print("iops_Samples:", iops_samples)


            multi_gw_match_bw = re.search(multi_gw_pattern_bw, line)
            if multi_gw_match_bw:
                fio=True
                #print(multi_gw_match_bw)
                bw_min = multi_gw_match_bw.group(1)
                bw_max = multi_gw_match_bw.group(2)
                bw_perc = multi_gw_match_bw.group(3)
                bw_avg = multi_gw_match_bw.group(4)
                bw_std = multi_gw_match_bw.group(5)
                bw_samples = multi_gw_match_bw.group(6)
            
                # print("bw_Min:", bw_min)
                # print("bw_Max:", bw_max)
                # print("bw_Per:", bw_perc)
                # print("bw_Avg:", bw_avg)
                # print("bw_Stdev:", bw_std)
                # print("bw_Samples:", bw_samples)
            

            multi_gw_match_clat = re.search(multi_gw_pattern_clat, line)  
            if multi_gw_match_clat:
                fio=True
                #print(multi_gw_match_clat)
                clat_min = multi_gw_match_clat.group(1)
                clat_max = multi_gw_match_clat.group(2)
                clat_avg = multi_gw_match_clat.group(3)
                clat_std = multi_gw_match_clat.group(4)
            
                # print("clat_Min:", clat_min)
                # print("clat_Max:", clat_max)
                # print("clat_Average:", clat_avg)
                # print("clat_Standard_Deviation:", clat_std)


            multi_gw_match_sum = re.search(multi_gw_pattern_sum, line)
            if multi_gw_match_sum:
                result=True
                #print(multi_gw_match_sum) 
                gateways = multi_gw_match_sum.group(1)
                iodepth = multi_gw_match_sum.group(2)
                bs = multi_gw_match_sum.group(3)
                workload= multi_gw_match_sum.group(4)
                request_type= multi_gw_match_sum.group(5)
                iops = multi_gw_match_sum.group(6)
                
                bw = multi_gw_match_sum.group(7)
                bw_mb = multi_gw_match_sum.group(8)
                total_bw = multi_gw_match_sum.group(9)
                total_time = multi_gw_match_sum.group(10)
            
                # print("Gateways:", gateway)
                # print("Iodepth:", iodepth)
                # print("Block Size:", bs)
                # print("Workload:", workload)
                # print("Operations:", request_type)
                # print("IOPS:", iops)
                # print("Bandwidth:", bw)
                # print("Bandwidth Converted:", bw_mb)
                # print("Total Size:", total_bw)
                # print("Total Time:", total_time)
                
                # print("IOPS:", iops)
                # print("Bandwidth:", bw)
                # print("Total Bandwidth:", bw_mb)
                # print("Total Size:", total_bw)

            


            




            match_clat_perc = re.finditer(pattern_clat_perc, line)
            if match_clat_perc:
                #print("Values extracted:")
                for match in match_clat_perc:
                    # print(f"{match.group(1)}: {match.group(2)}")
                    if match.group(1)=="1.00th":
                         clat_1th= match.group(2)
                        #  print("clat_1th", clat_1th)
                    elif match.group(1)=="50.00th":
                         clat_50th= match.group(2)
                    elif match.group(1)=="90.00th":
                         clat_90th= match.group(2)
                    elif match.group(1)=="99.99th":
                         clat_99th= match.group(2)

            match_iops = re.search(pattern_iops, line)
            if match_iops:
                fio=True
                iops_min = match_iops.group(1)
                iops_max = match_iops.group(2)
                iops_avg = match_iops.group(3)
                iops_std = match_iops.group(4)
                iops_samples = match_iops.group(5)
            
                # print("iops_Min:", iops_min)
                # print("iops_Max:", iops_max)
                # print("iops_Avg:", iops_avg)
                # print("iops_Stdev:", iops_std)
                # print("iops_Samples:", iops_samples)


            match_bw = re.search(pattern_bw, line)
            if match_bw:
                #print(match_bw)
                fio=True
                bw_min = match_bw.group(1)
                bw_max = match_bw.group(2)
                bw_perc = match_bw.group(3)
                bw_avg = match_bw.group(4)
                bw_std = match_bw.group(5)
                bw_samples = match_bw.group(6)
            
            #     print("bw_Min:", bw_min)
            #     print("bw_Max:", bw_max)
            #     print("bw_Per:", bw_perc)
            #     print("bw_Avg:", bw_avg)
            #     print("bw_Stdev:", bw_std)
            #     print("bw_Samples:", bw_samples)
            

            match_clat = re.search(pattern_clat, line)  
            if match_clat:
                #print(match_clat)
                fio=True
                clat_min = match_clat.group(1)
                clat_max = match_clat.group(2)
                clat_avg = match_clat.group(3)
                clat_std = match_clat.group(4)
            
            #     print("clat_Min:", clat_min)
            #     print("clat_Max:", clat_max)
            #     print("clat_Average:", clat_avg)
            #     print("clat_Standard_Deviation:", clat_std)

            match_sum = re.search(pattern_sum, line)
            if match_sum:
                result=True
                #print(match_sum)
                iodepth = match_sum.group(1)
                bs = match_sum.group(2)
                iops_limit = match_sum.group(3)
                workload= match_sum.group(4)
                request_type= match_sum.group(5)
                iops = match_sum.group(6)
                
                bw = match_sum.group(7)
                bw_mb = match_sum.group(8)
                total_bw = match_sum.group(9)
                total_time = match_sum.group(10)
            
                # print("Iodepth:", iodepth)
                # print("Block Size:", bs)
                # print("IOPS Limit:", iops_limit)
                # print("Workload:", workload)
                # print("Operations:", request_type)
                # print("IOPS:", iops)
                # print("Bandwidth:", bw)
                # print("Bandwidth Converted:", bw_mb)
                # print("Total Bandwidth:", total_bw)
                # print("Total Time:", total_time)


            match_sum2 = re.search(pattern_sum2, line)
            if match_sum2:
                #print(match_sum2)
                result=True
                iodepth = match_sum2.group(1)
                bs = match_sum2.group(2)
                workload= match_sum2.group(3)
                request_type= match_sum2.group(4)
                iops = match_sum2.group(5)
                
                bw = match_sum2.group(6)
                bw_mb = match_sum2.group(7)
                total_bw = match_sum2.group(8)
                total_time = match_sum2.group(9)
            
                # print("Iodepth:", iodepth)
                # print("Block Size:", bs)
                # print("IOPS Limit:", iops_limit)
                # print("Workload:", workload)
                # print("Operations:", request_type)
                # print("IOPS:", iops)
                # print("Bandwidth:", bw)
                # print("Bandwidth Converted:", bw_mb)
                # print("Total Bandwidth:", total_bw)
                # print("Total Time:", total_time)

            pattern_file = r"RESULT:\s*Filebench\s*/tmp/filebench/(\S+).f:(\S+):\s*IO Summary:\s*(\S+)\s*ops\s*(\S+)\s*ops/s\s*(\S+)/(\S+)\s*rd/wr\s*(\S+) (\S+)"
            pattern_operation = r"(\S+)\s*(\S+)ops\s*(\S+)ops/s\s*(\S+)\s*(\S+) "
            
                
            match_operation = re.search(pattern_operation, line)
            if match_operation:
                ops_bool=True
                operation_type= match_operation.group(1)
                operation_iops= match_operation.group(2)
                operation_iops_per_sec= match_operation.group(3)
                operation_bw= match_operation.group(4)
                operation_bw_per_op= match_operation.group(5)
                # print("operation_type:", operation_type)
                # print("operation_iops:", operation_iops)
                # print("operation_iops_per_sec:", operation_iops_per_sec)
                # print("operation_bw:", operation_bw)
                # print("operation_bw_per_op:", operation_bw_per_op)
                if file_type=="fileserver":
                    fileserver_ops_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, operation_type, operation_iops, operation_iops_per_sec, operation_bw, operation_bw_per_op])
                elif file_type=="oltp":
                    oltp_ops_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, operation_type, operation_iops, operation_iops_per_sec, operation_bw, operation_bw_per_op])
                elif file_type=="randomwrite":
                    randomwrite_ops_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, operation_type, operation_iops, operation_iops_per_sec, operation_bw, operation_bw_per_op])
                elif file_type=="varmail":
                    varmail_ops_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, operation_type, operation_iops, operation_iops_per_sec, operation_bw, operation_bw_per_op])

            match_filebench = re.search(pattern_file, line)
            if match_filebench:
                file_bool=True
                file_type= match_filebench.group(1)
                file_time= match_filebench.group(2)
                file_total_ops = match_filebench.group(3)
                file_ops_per_second = match_filebench.group(4)
                file_read = match_filebench.group(5)
                file_write= match_filebench.group(6)
                file_bw = match_filebench.group(7)
                file_bw_per_ops = match_filebench.group(8)
                # print("file_type:", file_type)
                # print("file_time:", file_time)
                # print("file_total_ops:", file_total_ops)
                # print("file_ops_per_second:", file_ops_per_second)
                # print("file_read:", file_read)
                # print("file_write:", file_write)
                # print("file_bw:", file_bw)
                # print("file_bw_per_ops:", file_bw_per_ops)
                single_filebench_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, file_time, file_total_ops , file_ops_per_second , file_read , file_write, file_bw , file_bw_per_ops])
                if file_type=="fileserver":
                    file_type="oltp"
                elif file_type=="oltp":
                    file_type="randomwrite"
                # elif file_type=="oltp" and disk_type=="rbd":
                #     file_type="varmail"
                elif file_type=="randomwrite":
                    file_type="varmail"

            if(fio and result):
                print("INSIDE FIO: " + workload)
                if 'multigw' in file_name:
                    print("INSIDE MULTIGW: " + workload)
                    multi_gw_fio_csv_writer.writerow([disk_type, pool_type, cache_size, iops_min, iops_max, iops_avg, iops_std, iops_samples, bw_min, bw_max, bw_perc, bw_avg, bw_std, bw_samples, clat_min, clat_max, clat_avg, clat_std, iodepth, bs, gateways, workload, request_type, iops, bw, bw_mb, total_bw, total_time, clat_1th, clat_50th, clat_90th, clat_99th])  
                elif ('multi'  in file_name and not 'gw' in file_name):
                    print("INSIDE MULTI: " + workload)
                    multi_fio_csv_writer.writerow([disk_type, pool_type, cache_size, iops_min, iops_max, iops_avg, iops_std, iops_samples, bw_min, bw_max, bw_perc, bw_avg, bw_std, bw_samples, clat_min, clat_max, clat_avg, clat_std, iodepth, bs, disks, workload, request_type, iops, bw, bw_mb, total_bw, total_time, clat_1th, clat_50th, clat_90th, clat_99th])
                else:
                    single_fio_csv_writer.writerow([disk_type, pool_type, cache_size, iops_min, iops_max, iops_avg, iops_std, iops_samples, bw_min, bw_max, bw_perc, bw_avg, bw_std, bw_samples, clat_min, clat_max, clat_avg, clat_std, iodepth, bs, iops_limit, workload, request_type, iops, bw, bw_mb, total_bw, total_time, clat_1th, clat_50th, clat_90th, clat_99th])
                fio=False
                result=False

            if(ops_bool and file_bool):
                print("INSIDE FILE: " + file_type)
                # single_filebench_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, file_time, file_total_ops , file_ops_per_second , file_read , file_write, file_bw , file_bw_per_ops])
                # if file_type=="fileserver":
                #     fileserver_ops_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, operation_type, operation_iops, operation_iops_per_sec, operation_bw, operation_bw_per_op])
                # elif file_type=="oltp":
                #     oltp_ops_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, operation_type, operation_iops, operation_iops_per_sec, operation_bw, operation_bw_per_op])
                # elif file_type=="randomwrite":
                #     randomwrite_ops_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, operation_type, operation_iops, operation_iops_per_sec, operation_bw, operation_bw_per_op])
                # elif file_type=="varmail":
                #     varmail_ops_csv_writer.writerow([disk_type, pool_type, cache_size, file_type, operation_type, operation_iops, operation_iops_per_sec, operation_bw, operation_bw_per_op])
                ops_bool=False
                file_bool=False

multi_fio_file.close()
multi_gw_fio_file.close()
single_fio_file.close()
single_filebench_file.close()
fileserver_ops_file.close()
oltp_ops_file.close()
randomwrite_ops_file.close()
varmail_ops_file.close()
            