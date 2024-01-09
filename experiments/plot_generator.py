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
result_dir = os.path.join(directory, 'results/28_dec_res')
graph_dir = os.path.join(result_dir, 'graphs')
if not os.path.exists(graph_dir):
    os.makedirs(graph_dir)
fio_output_file= os.path.join(graph_dir, 'fio_output.csv')
fio_plot_file= os.path.join(graph_dir, 'fio_plot_1')
fio_output_file_2= os.path.join(graph_dir, 'fio_output_all.csv')
fio_plot_file_2= os.path.join(graph_dir, 'fio_plot_2')
filebench_output_file= os.path.join(graph_dir, 'filebench_output.csv')
filebench_plot_file= os.path.join(graph_dir, 'filebench_plot_1')

##### 20G Results #####


df = pd.read_csv(fio_output_file_2)

bs='4k'
qd=256
cs='20gb'
pt='ssd'
dt='lsvd'




#condition for lsvd_ssd
conditions_lsvd_ssd_rr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randread') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_ssd_rw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randwrite') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_ssd_sr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'read') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_ssd_sw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'write') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)




pt ='hdd'
#confition for lsvd_hdd
conditions_lsvd_hdd_rr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randread') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_hdd_rw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randwrite') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_hdd_sr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'read') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_hdd_sw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'write') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)




dt='rbd'
pt='ssd'
cs='none'
#condition for rbd_ssd
conditions_rbd_ssd_rr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randread') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_ssd_rw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randwrite') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_ssd_sr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'read') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_ssd_sw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'write') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

pt='hdd'
#confition for rbd_hdd
conditions_rbd_hdd_rr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randread') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_hdd_rw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randwrite') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_hdd_sr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'read') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_hdd_sw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'write') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)


bw_lsvd_ssd_rr=df[conditions_lsvd_ssd_rr]
bw_lsvd_ssd_rr=bw_lsvd_ssd_rr['bw'].iloc[0]
print(bw_lsvd_ssd_rr)

bw_rbd_ssd_rr=df[conditions_rbd_ssd_rr]
bw_rbd_ssd_rr=bw_rbd_ssd_rr['bw'].iloc[0]
#print(bw_rbd_ssd_rr)

bw_lsvd_ssd_rw=df[conditions_lsvd_ssd_rw]
bw_lsvd_ssd_rw=bw_lsvd_ssd_rw['bw'].iloc[0]
#print(bw_lsvd_ssd_rw)

bw_rbd_ssd_rw=df[conditions_rbd_ssd_rw]
bw_rbd_ssd_rw=bw_rbd_ssd_rw['bw'].iloc[0]
#print(bw_rbd_ssd_rw)

bw_lsvd_ssd_sr=df[conditions_lsvd_ssd_sr]
bw_lsvd_ssd_sr=bw_lsvd_ssd_sr['bw'].iloc[0]
#print(bw_lsvd_ssd_sr)

bw_rbd_ssd_sr=df[conditions_rbd_ssd_sr]
bw_rbd_ssd_sr=bw_rbd_ssd_sr['bw'].iloc[0]
#print(bw_rbd_ssd_sr)

bw_lsvd_ssd_sw=df[conditions_lsvd_ssd_sw]
bw_lsvd_ssd_sw=bw_lsvd_ssd_sw['bw'].iloc[0]
#print(bw_lsvd_ssd_sw)

bw_rbd_ssd_sw=df[conditions_rbd_ssd_sw]
bw_rbd_ssd_sw=bw_rbd_ssd_sw['bw'].iloc[0]
#print(bw_rbd_ssd_sw)



bw_ssd_array=[bw_lsvd_ssd_rr, bw_rbd_ssd_rr, bw_lsvd_ssd_rw, bw_rbd_ssd_rw,
              bw_lsvd_ssd_sr, bw_rbd_ssd_sr, bw_lsvd_ssd_sw, bw_rbd_ssd_sw ]
print(bw_ssd_array)



bw_lsvd_hdd_rr=df[conditions_lsvd_hdd_rr]
bw_lsvd_hdd_rr=bw_lsvd_hdd_rr['bw'].iloc[0]
#print(bw_lsvd_hdd_rr)

bw_rbd_hdd_rr=df[conditions_rbd_hdd_rr]
bw_rbd_hdd_rr=bw_rbd_hdd_rr['bw'].iloc[0]
#print(bw_rbd_hdd_rr)

bw_lsvd_hdd_rw=df[conditions_lsvd_hdd_rw]
bw_lsvd_hdd_rw=bw_lsvd_hdd_rw['bw'].iloc[0]
#print(bw_lsvd_hdd_rw)

bw_rbd_hdd_rw=df[conditions_rbd_hdd_rw]
bw_rbd_hdd_rw=bw_rbd_hdd_rw['bw'].iloc[0]
#print(bw_rbd_hdd_rw)

bw_lsvd_hdd_sr=df[conditions_lsvd_hdd_sr]
bw_lsvd_hdd_sr=bw_lsvd_hdd_sr['bw'].iloc[0]
#print(bw_lsvd_hdd_sr)

bw_rbd_hdd_sr=df[conditions_rbd_hdd_sr]
bw_rbd_hdd_sr=bw_rbd_hdd_sr['bw'].iloc[0]
#print(bw_rbd_hdd_sr)

bw_lsvd_hdd_sw=df[conditions_lsvd_hdd_sw]
bw_lsvd_hdd_sw=bw_lsvd_hdd_sw['bw'].iloc[0]
#print(bw_lsvd_hdd_sw)

bw_rbd_hdd_sw=df[conditions_rbd_hdd_sw]
bw_rbd_hdd_sw=bw_rbd_hdd_sw['bw'].iloc[0]
#print(bw_rbd_hdd_sw)

bw_hdd_array=[bw_lsvd_hdd_rr, bw_rbd_hdd_rr, bw_lsvd_hdd_rw, bw_rbd_hdd_rw,
              bw_lsvd_hdd_sr, bw_rbd_hdd_sr, bw_lsvd_hdd_sw, bw_rbd_hdd_sw ]
print(bw_hdd_array)

lsvd_ssd_bw=[bw_lsvd_ssd_rr, bw_lsvd_ssd_rw, bw_lsvd_ssd_sr, bw_lsvd_ssd_sw]
rbd_ssd_bw=[bw_rbd_ssd_rr, bw_rbd_ssd_rw, bw_rbd_ssd_sr, bw_rbd_ssd_sw]
print(lsvd_ssd_bw, rbd_ssd_bw)

lsvd_hdd_bw=[bw_lsvd_hdd_rr, bw_lsvd_hdd_rw, bw_lsvd_hdd_sr, bw_lsvd_hdd_sw]
rbd_hdd_bw=[bw_rbd_hdd_rr, bw_rbd_hdd_rw, bw_rbd_hdd_sr, bw_rbd_hdd_sw]
print(lsvd_hdd_bw, rbd_hdd_bw)
xlabel=['rand read', 'rand write', 'seq read', 'seq write']



# fio_x =  df.loc[conditions_lsvd_ssd].head(5)['iodepth'].to_numpy() 
# # set width of bar 
barWidth = 0.30
fig = plt.subplots(figsize =(12, 8)) 

 
 
br1 = np.arange(len(lsvd_ssd_bw)) 
br2 = [x + barWidth for x in br1] 
# br3 = [x + barWidth for x in br2] 
# br4 = [x + barWidth for x in br3]
 

plt.bar(br1, lsvd_ssd_bw, color ='lightseagreen', width = barWidth, 
        edgecolor ='grey', label ='lsvd') 
plt.bar(br2, rbd_ssd_bw, color ='orange', width = barWidth, 
        edgecolor ='grey', label ='rbd') 
# plt.bar(br3, rbd_hdd_bw, color ='salmon', width = barWidth, 
#         edgecolor ='grey', label ='rbd_hdd')
# plt.bar(br4, rbd_ssd_bw, color ='khaki', width = barWidth, 
#         edgecolor ='grey', label ='rbd_ssd') 
 
# Adding Xticks 
plt.xlabel('Experiment', fontweight ='bold', fontsize = 20) 
plt.ylim(0, 300)
plt.ylabel('Throughput (MB/s)', fontweight ='bold', fontsize = 20) 
plt.xticks([r + barWidth for r in range(len(lsvd_hdd_bw))], 
        xlabel, fontsize=20)
 
plt.legend()
description = f"FIO performance LSVD vs RBD for SSD backend. 80GB volume, 20GB cache, 4K blocksize, queue depth of 256"
# plt.text(0.5, -0.15, description, ha='center', va='center', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.9), transform=plt.gca().transAxes)
# plt.subplots_adjust(bottom=0.2) 
plt.savefig(os.path.join(graph_dir, 'request_bw_ssd_20gb.png'))
plt.show()

barWidth = 0.30
fig = plt.subplots(figsize =(12, 8)) 

 
# # # Set position of bar on X axis 
br1 = np.arange(len(lsvd_hdd_bw)) 
br2 = [x + barWidth for x in br1] 
# br3 = [x + barWidth for x in br2] 
# br4 = [x + barWidth for x in br3]
 
# # Make the plot
plt.bar(br1, lsvd_hdd_bw, color ='lightseagreen', width = barWidth, 
        edgecolor ='grey', label ='lsvd') 
plt.bar(br2, rbd_hdd_bw, color ='orange', width = barWidth, 
        edgecolor ='grey', label ='rbd') 
# plt.bar(br3, rbd_hdd_bw, color ='salmon', width = barWidth, 
#         edgecolor ='grey', label ='rbd_hdd')
# plt.bar(br4, rbd_ssd_bw, color ='khaki', width = barWidth, 
#         edgecolor ='grey', label ='rbd_ssd') 
 
# Adding Xticks 
plt.xlabel('Experiment', fontweight ='bold', fontsize = 20) 
plt.ylabel('Throughput (MB/s)', fontweight ='bold', fontsize = 20) 
plt.ylim(0, 300)
plt.xticks([r + barWidth for r in range(len(lsvd_hdd_bw))], 
        xlabel, fontsize=20)
 
plt.legend()
description = f"FIO performance LSVD vs RBD for Hard Disk backend. 80GB volume, 20GB cache, 4K blocksize, queue depth of 256"
#plt.text(0.5, -0.15, description, ha='center', va='center', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.9), transform=plt.gca().transAxes)
#plt.subplots_adjust(bottom=0.2) 
plt.savefig(os.path.join(graph_dir, 'request_bw_hdd_20gb.png'))
plt.show()


df = pd.read_csv(fio_output_file_2)

bs='4k'
qd=256
cs='240gb'
pt='ssd'
dt='lsvd'




#condition for lsvd_ssd
conditions_lsvd_ssd_rr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randread') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_ssd_rw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randwrite') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_ssd_sr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'read') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_ssd_sw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'write') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)




pt ='hdd'
#confition for lsvd_hdd
conditions_lsvd_hdd_rr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randread') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_hdd_rw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randwrite') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_hdd_sr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'read') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_lsvd_hdd_sw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'write') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)




dt='rbd'
pt='ssd'
cs='none'
#condition for rbd_ssd
conditions_rbd_ssd_rr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randread') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_ssd_rw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randwrite') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_ssd_sr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'read') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_ssd_sw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'write') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

pt='hdd'
#confition for rbd_hdd
conditions_rbd_hdd_rr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randread') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_hdd_rw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'randwrite') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_hdd_sr = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'read') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)

conditions_rbd_hdd_sw = (
    (df['pool_type'] == pt) & 
    (df['disk_type'] == dt) & 
    (df['cache_size'] == cs) & 
    (df['workload'] == 'write') &
    (df['blocksize'] == bs) &
    (df['iodepth']==qd)
)


bw_lsvd_ssd_rr=df[conditions_lsvd_ssd_rr]
bw_lsvd_ssd_rr=bw_lsvd_ssd_rr['bw'].iloc[0]
#print(bw_lsvd_ssd_rr)

bw_rbd_ssd_rr=df[conditions_rbd_ssd_rr]
bw_rbd_ssd_rr=bw_rbd_ssd_rr['bw'].iloc[0]
#print(bw_rbd_ssd_rr)

bw_lsvd_ssd_rw=df[conditions_lsvd_ssd_rw]
bw_lsvd_ssd_rw=bw_lsvd_ssd_rw['bw'].iloc[0]
#print(bw_lsvd_ssd_rw)

bw_rbd_ssd_rw=df[conditions_rbd_ssd_rw]
bw_rbd_ssd_rw=bw_rbd_ssd_rw['bw'].iloc[0]
#print(bw_rbd_ssd_rw)

bw_lsvd_ssd_sr=df[conditions_lsvd_ssd_sr]
bw_lsvd_ssd_sr=bw_lsvd_ssd_sr['bw'].iloc[0]
#print(bw_lsvd_ssd_sr)

bw_rbd_ssd_sr=df[conditions_rbd_ssd_sr]
bw_rbd_ssd_sr=bw_rbd_ssd_sr['bw'].iloc[0]
#print(bw_rbd_ssd_sr)

bw_lsvd_ssd_sw=df[conditions_lsvd_ssd_sw]
bw_lsvd_ssd_sw=bw_lsvd_ssd_sw['bw'].iloc[0]
#print(bw_lsvd_ssd_sw)

bw_rbd_ssd_sw=df[conditions_rbd_ssd_sw]
bw_rbd_ssd_sw=bw_rbd_ssd_sw['bw'].iloc[0]
#print(bw_rbd_ssd_sw)



bw_ssd_array=[bw_lsvd_ssd_rr, bw_rbd_ssd_rr, bw_lsvd_ssd_rw, bw_rbd_ssd_rw,
              bw_lsvd_ssd_sr, bw_rbd_ssd_sr, bw_lsvd_ssd_sw, bw_rbd_ssd_sw ]
print(bw_ssd_array)



bw_lsvd_hdd_rr=df[conditions_lsvd_hdd_rr]
bw_lsvd_hdd_rr=bw_lsvd_hdd_rr['bw'].iloc[0]
#print(bw_lsvd_hdd_rr)

bw_rbd_hdd_rr=df[conditions_rbd_hdd_rr]
bw_rbd_hdd_rr=bw_rbd_hdd_rr['bw'].iloc[0]
#print(bw_rbd_hdd_rr)

bw_lsvd_hdd_rw=df[conditions_lsvd_hdd_rw]
bw_lsvd_hdd_rw=bw_lsvd_hdd_rw['bw'].iloc[0]
#print(bw_lsvd_hdd_rw)

bw_rbd_hdd_rw=df[conditions_rbd_hdd_rw]
bw_rbd_hdd_rw=bw_rbd_hdd_rw['bw'].iloc[0]
#print(bw_rbd_hdd_rw)

bw_lsvd_hdd_sr=df[conditions_lsvd_hdd_sr]
bw_lsvd_hdd_sr=bw_lsvd_hdd_sr['bw'].iloc[0]
#print(bw_lsvd_hdd_sr)

bw_rbd_hdd_sr=df[conditions_rbd_hdd_sr]
bw_rbd_hdd_sr=bw_rbd_hdd_sr['bw'].iloc[0]
#print(bw_rbd_hdd_sr)

bw_lsvd_hdd_sw=df[conditions_lsvd_hdd_sw]
bw_lsvd_hdd_sw=bw_lsvd_hdd_sw['bw'].iloc[0]
#print(bw_lsvd_hdd_sw)

bw_rbd_hdd_sw=df[conditions_rbd_hdd_sw]
bw_rbd_hdd_sw=bw_rbd_hdd_sw['bw'].iloc[0]
#print(bw_rbd_hdd_sw)

bw_hdd_array=[bw_lsvd_hdd_rr, bw_rbd_hdd_rr, bw_lsvd_hdd_rw, bw_rbd_hdd_rw,
              bw_lsvd_hdd_sr, bw_rbd_hdd_sr, bw_lsvd_hdd_sw, bw_rbd_hdd_sw ]
print(bw_hdd_array)

lsvd_ssd_bw=[bw_lsvd_ssd_rr, bw_lsvd_ssd_rw, bw_lsvd_ssd_sr, bw_lsvd_ssd_sw]
rbd_ssd_bw=[bw_rbd_ssd_rr, bw_rbd_ssd_rw, bw_rbd_ssd_sr, bw_rbd_ssd_sw]
print(lsvd_ssd_bw, rbd_ssd_bw)

lsvd_hdd_bw=[bw_lsvd_hdd_rr, bw_lsvd_hdd_rw, bw_lsvd_hdd_sr, bw_lsvd_hdd_sw]
rbd_hdd_bw=[bw_rbd_hdd_rr, bw_rbd_hdd_rw, bw_rbd_hdd_sr, bw_rbd_hdd_sw]
print(lsvd_hdd_bw, rbd_hdd_bw)
xlabel=['rand read', 'rand write', 'seq read', 'seq write']

# fio_x =  df.loc[conditions_lsvd_ssd].head(5)['iodepth'].to_numpy() 

barWidth = 0.30
fig = plt.subplots(figsize =(12, 8)) 

 
 
br1 = np.arange(len(lsvd_ssd_bw)) 
br2 = [x + barWidth for x in br1] 
# br3 = [x + barWidth for x in br2] 
# br4 = [x + barWidth for x in br3]
 
# # Make the plot
plt.bar(br1, lsvd_ssd_bw, color ='lightseagreen', width = barWidth, 
        edgecolor ='grey', label ='lsvd') 
plt.bar(br2, rbd_ssd_bw, color ='orange', width = barWidth, 
        edgecolor ='grey', label ='rbd') 
# plt.bar(br3, rbd_hdd_bw, color ='salmon', width = barWidth, 
#         edgecolor ='grey', label ='rbd_hdd')
# plt.bar(br4, rbd_ssd_bw, color ='khaki', width = barWidth, 
#         edgecolor ='grey', label ='rbd_ssd') 
  
plt.xlabel('Experiment', fontweight ='bold', fontsize = 20) 
plt.ylabel('Throughput (MB/s)', fontweight ='bold', fontsize = 20) 
plt.ylim(0, 300)
plt.xticks([r + barWidth for r in range(len(lsvd_hdd_bw))], 
        xlabel, fontsize=20)
 
plt.legend()
description = f"FIO Performance LSVD vs RBD for SSD backend. 80GB volume, 240GB cache, 4K blocksize, queue depth of 256"
#plt.text(0.5, -0.15, description, ha='center', va='center', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.9), transform=plt.gca().transAxes)
#plt.subplots_adjust(bottom=0.2) 
plt.savefig(os.path.join(graph_dir, 'request_bw_ssd_240gb.png'))
plt.show()

barWidth = 0.30
fig = plt.subplots(figsize =(12, 8)) 

 
br1 = np.arange(len(lsvd_hdd_bw)) 
br2 = [x + barWidth for x in br1] 
# br3 = [x + barWidth for x in br2] 
# br4 = [x + barWidth for x in br3]
 

plt.bar(br1, lsvd_hdd_bw, color ='lightseagreen', width = barWidth, 
        edgecolor ='grey', label ='lsvd') 
plt.bar(br2, rbd_hdd_bw, color ='orange', width = barWidth, 
        edgecolor ='grey', label ='rbd') 
# plt.bar(br3, rbd_hdd_bw, color ='salmon', width = barWidth, 
#         edgecolor ='grey', label ='rbd_hdd')
# plt.bar(br4, rbd_ssd_bw, color ='khaki', width = barWidth, 
#         edgecolor ='grey', label ='rbd_ssd') 
 
plt.xlabel('Experiment', fontweight ='bold', fontsize = 20) 
plt.ylabel('Throughput (MB/s)', fontweight ='bold', fontsize = 20)
plt.ylim(0, 300) 
plt.xticks([r + barWidth for r in range(len(lsvd_hdd_bw))], 
        xlabel, fontsize=20)
 
plt.legend()
description = f"FIO Performance LSVD vs RBD for Hard Disk backend. 80GB volume, 240GB cache, 4K blocksize, queue depth of 256"
#plt.text(0.5, -0.15, description, ha='center', va='center', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.9), transform=plt.gca().transAxes)
#plt.subplots_adjust(bottom=0.2) 
plt.savefig(os.path.join(graph_dir, 'request_bw_hdd_240gb.png'))
plt.show()



##### FILEBENCH  ####


df = pd.read_csv(filebench_output_file)


filtered_df = df[(df['pool_type'] == 'ssd') & (df['disk_type'] == 'lsvd') & (df['cache_size'] == '240gb')]
lsvd_ssd = filtered_df[['fileserver_throughput', 'oltp_throughput', 'varmail_throughput']].iloc[0].to_numpy()

filtered_df = df[(df['pool_type'] == 'hdd') & (df['disk_type'] == 'lsvd') & (df['cache_size'] == '240gb')]
lsvd_hdd = filtered_df[['fileserver_throughput', 'oltp_throughput', 'varmail_throughput']].iloc[0].to_numpy()

filtered_df = df[(df['pool_type'] == 'ssd') & (df['disk_type'] == 'rbd') & (df['cache_size'] == 'none')]
rbd_ssd = filtered_df[['fileserver_throughput', 'oltp_throughput', 'varmail_throughput']].iloc[0].to_numpy()

filtered_df = df[(df['pool_type'] == 'hdd') & (df['disk_type'] == 'rbd') & (df['cache_size'] == 'none')]
rbd_hdd = filtered_df[['fileserver_throughput', 'oltp_throughput', 'varmail_throughput']].iloc[0].to_numpy()

# Print the result
print(lsvd_hdd)
print(lsvd_ssd)
print(rbd_hdd)
print(rbd_ssd)


file_x = ['fileserver','oltp','varmail'] 
# set width of bar 
barWidth = 0.20
fig = plt.subplots(figsize =(12, 8)) 

 
br1 = np.arange(len(lsvd_hdd)) 
br2 = [x + barWidth for x in br1] 
br3 = [x + barWidth for x in br2] 
# br4 = [x + barWidth for x in br3]
 

plt.bar(br1, lsvd_hdd, color ='lightseagreen', width = barWidth, 
        edgecolor ='grey', label ='lsvd_hdd') 
plt.bar(br2, lsvd_ssd, color ='orange', width = barWidth, 
        edgecolor ='grey', label ='lsvd_ssd') 
plt.bar(br3, rbd_ssd, color ='salmon', width = barWidth, 
        edgecolor ='grey', label ='rbd_ssd')
# plt.bar(br4, rbd_ssd, color ='khaki', width = barWidth, 
#         edgecolor ='grey', label ='rbd_ssd') 
 
# Adding Xticks 
plt.xlabel('Filebench Workload', fontweight ='bold', fontsize = 20)
plt.ylim(0, 1800)  
plt.ylabel('Throughput (MB/s)', fontweight ='bold', fontsize = 20) 
plt.xticks([r + barWidth for r in range(len(lsvd_hdd))], 
        file_x, fontsize=20)
 
plt.legend()
description = f"Filebench throughput, LSVD on Hard Disk vs RBD on SSD backend. 80GB volume, 240GB cache"
#plt.text(0.5, -0.15, description, ha='center', va='center', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.9), transform=plt.gca().transAxes)
#plt.subplots_adjust(bottom=0.2) 
plt.savefig(os.path.join(graph_dir, 'filebench_240g.png'))
plt.show() 


df = pd.read_csv(filebench_output_file)


filtered_df = df[(df['pool_type'] == 'ssd') & (df['disk_type'] == 'lsvd') & (df['cache_size'] == '20gb')]
lsvd_ssd = filtered_df[['fileserver_throughput','oltp_throughput', 'varmail_throughput']].iloc[0].to_numpy()

filtered_df = df[(df['pool_type'] == 'hdd') & (df['disk_type'] == 'lsvd') & (df['cache_size'] == '20gb')]
lsvd_hdd = filtered_df[['fileserver_throughput', 'oltp_throughput', 'varmail_throughput']].iloc[0].to_numpy()

filtered_df = df[(df['pool_type'] == 'ssd') & (df['disk_type'] == 'rbd') & (df['cache_size'] == 'none')]
rbd_ssd = filtered_df[['fileserver_throughput', 'oltp_throughput', 'varmail_throughput']].iloc[0].to_numpy()

filtered_df = df[(df['pool_type'] == 'hdd') & (df['disk_type'] == 'rbd') & (df['cache_size'] == 'none')]
rbd_hdd = filtered_df[['fileserver_throughput', 'oltp_throughput', 'varmail_throughput']].iloc[0].to_numpy()

# Print the result
print(lsvd_hdd)
print(lsvd_ssd)
print(rbd_hdd)
print(rbd_ssd)


file_x = ['fileserver','oltp','varmail'] 
# set width of bar 
barWidth = 0.20
fig = plt.subplots(figsize =(12, 8)) 

 
# Set position of bar on X axis 
br1 = np.arange(len(lsvd_hdd)) 
br2 = [x + barWidth for x in br1] 
br3 = [x + barWidth for x in br2] 
# br4 = [x + barWidth for x in br3]
 
# Make the plot
plt.bar(br1, lsvd_hdd, color ='lightseagreen', width = barWidth, 
        edgecolor ='grey', label ='lsvd_hdd') 
plt.bar(br2, lsvd_ssd, color ='orange', width = barWidth, 
        edgecolor ='grey', label ='lsvd_ssd') 
plt.bar(br3, rbd_ssd, color ='salmon', width = barWidth, 
        edgecolor ='grey', label ='rbd_ssd')
# plt.bar(br4, lsvd_ssd, color ='khaki', width = barWidth, 
#         edgecolor ='grey', label ='lsvd_ssd') 
 
# Adding Xticks 
plt.xlabel('Filebench Workload', fontweight ='bold', fontsize = 20) 
plt.ylim(0, 1800) 
plt.ylabel('Throughput (MB/s)', fontweight ='bold', fontsize = 20) 
plt.xticks([r + barWidth for r in range(len(lsvd_hdd))], 
        file_x, fontsize=20)
 
plt.legend()
description = f"Filebench throughput, LSVD on Hard Disk vs RBD on SSD backend. 80GB volume, 20GB cache"
#plt.text(0.5, -0.15, description, ha='center', va='center', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.9), transform=plt.gca().transAxes)
#plt.subplots_adjust(bottom=0.2) 
plt.savefig(os.path.join(graph_dir, 'filebench_20g.png'))
plt.show() 

