"""Independent shell oracle: enumerate four-symbol halves, combine by energy."""
from bisect import bisect_right
import itertools
class ShellReference:
    def __init__(self,m):
        self.total=m**8;self.groups=[[] for _ in range(4*(m-1)+1)]
        for rings in itertools.product(range(m),repeat=4):self.groups[sum(rings)].append(rings)
        for group in self.groups:group.sort(key=lambda r:(sum(r[:2]),r[2],r[0]))
        self.starts=[];self.buckets=[];start=0
        for energy in range(8*(m-1)+1):
            for left in range(len(self.groups)):
                right=energy-left
                if 0<=right<len(self.groups):
                    count=len(self.groups[left])*len(self.groups[right])
                    if count:self.starts.append(start);self.buckets.append((left,right));start+=count
        assert start==self.total
    def __getitem__(self,index):
        if not isinstance(index,int) or not 0<=index<self.total:raise IndexError(index)
        bucket=bisect_right(self.starts,index)-1;left,right=self.buckets[bucket]
        remainder=index-self.starts[bucket];count=len(self.groups[left])
        return self.groups[left][remainder%count]+self.groups[right][remainder//count]
if __name__=='__main__':
    for m in [1,2,3]:
        expected=sorted(itertools.product(range(m),repeat=8),key=lambda r:(sum(r),sum(r[:4]),sum(r[4:6]),r[6],r[4],sum(r[:2]),r[2],r[0]))
        oracle=ShellReference(m)
        assert [oracle[i] for i in range(m**8)]==expected
    print('PASS: half-enumeration oracle agrees with exhaustive M=1/2/3 ordering')
