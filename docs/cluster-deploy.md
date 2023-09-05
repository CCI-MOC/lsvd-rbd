On machine 1:

```
# bootstrap the cluster
cephadm bootstrap --mon-ip 10.1.0.1 --cluster-network 10.1.0.0/24

# copy ceph's key to other hosts
ssh-copy-id -f -i /etc/ceph/ceph.pub root@dl380p-2
ssh-copy-id -f -i /etc/ceph/ceph.pub root@dl380p-3
ssh-copy-id -f -i /etc/ceph/ceph.pub root@dl380p-4
ssh-copy-id -f -i /etc/ceph/ceph.pub root@dl380p-5

# add the hosts to the cluster
ceph orch host add dl380p-2 10.1.0.2
ceph orch host add dl380p-3 10.1.0.3
ceph orch host add dl380p-4 10.1.0.4
```

Adding OSDs

- See all available devices `ceph orch device ls`
- Add OSDs `ceph orch daemon add osd dl380p-1:/dev/disk/by-id/$disk`

Create pools

- Create SSD only ruleset: `ceph osd crush rule create-replicated replicated_ssd default ssd`
- `ceph osd pool create $poolname $pgnum $pgpnum replicated replicated_ssd`
