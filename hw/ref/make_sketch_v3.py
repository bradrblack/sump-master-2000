K=10.5; OX=40; OY=60
o=[]
def R(x,y,w,h,fc,label="",dash=False,fs=11,op=1,oy=0):
    o.append(f'<rect x="{OX+x*K}" y="{OY+oy+y*K}" width="{w*K}" height="{h*K}" fill="{fc}" fill-opacity="{op}" stroke="#222" stroke-width="1.5"'+(' stroke-dasharray="6 4"' if dash else '')+'/>')
    ls=label.split("\n") if label else []
    cy=OY+oy+(y+h/2)*K-(len(ls)-1)*fs*0.6+4
    for i,l in enumerate(ls): o.append(f'<text x="{OX+(x+w/2)*K}" y="{cy+i*fs*1.25}" font-size="{fs}" text-anchor="middle" fill="#111">{l}</text>')
def T(x,y,s,fs=12,c="#111",a="middle",oy=0): o.append(f'<text x="{OX+x*K}" y="{OY+oy+y*K}" font-size="{fs}" text-anchor="{a}" fill="{c}">{s}</text>')
L=104
T(0,-3,"TOP VIEW - PCB 24 x 100 mm, case about 33 x 108 (to scale)",14,a="start")
R(-4,-4.5,L+8,33,"#f4f4f4",dash=True)
R(0,0,100,24,"#e4f0df")
R(-1.5,3,23.5,17.8,"#cfe3ff","ESP32-C3\nSuperMini\nUSB-C at left")
R(26,2,17,20,"#ffe8b3","GY-346\nADXL346\n(flat, in socket)",fs=10)
R(46,6,17,12,"#ffd1d1","AHT20\n(flat, in socket)",fs=10)
R(23.3,0,1.4,24,"#fff",dash=True)
T(24,-1.2,"slot",9)
R(64,0,2,24,"#fff",dash=True); T(65,-1.2,"slot",9)
R(67,9.5,10,2.54,"#dddddd","1x4 lead",fs=9)
R(59,-2.5,41.5,29,"#bfe0ff","",dash=True,op=.4)
T(80,-3.3,"SR04 board under the lid (41.5 x 29)",10)
R(88,6.5,9,11.5,"#ffffff","JST plug",dash=True,fs=9)
o.append(f'<path d="M {OX+97*K} {OY+12*K} L {OX+104*K} {OY+12*K}" stroke="#222" stroke-width="5"/>')
T(104,16,"probe cable",10,a="middle")
for x,y in [(24.5,5),(44.5,5),(65,6),(75,16)]:
    o.append(f'<circle cx="{OX+x*K}" cy="{OY+y*K}" r="{1.4*K}" fill="#555" stroke="#222"/>')
T(50,27.8,"470uF caps (dark circles): ESP, ADXL, AHT20, SR04 header",9)
oy=360
T(0,oy/K+OY/K-4+2,"SIDE VIEW (to scale)",14,a="start")
R(-4,0,L+8,24,"#f4f4f4",dash=True,oy=oy)
R(-4,0,L+8,2,"#bbbbbb","lid",oy=oy,fs=10)
R(0,22-1.6,100,1.6,"#7ab57a","",oy=oy)
R(-1.5,22-1.6-8,23.5,8,"#cfe3ff","ESP32",oy=oy,fs=10)
R(26,22-1.6-11,17,11,"#ffe8b3","ADXL",oy=oy,fs=9)
R(46,22-1.6-11,17,11,"#ffd1d1","AHT20",oy=oy,fs=9)
R(59,2,41.5,1.2,"#9fd0ff","SR04 PCB on lid, parts down",oy=oy,fs=9)
R(70,3.2,10,15,"#999","tall parts",oy=oy,fs=9)
w=h=1250
open(__file__.rsplit('/',1)[0]+'/layout3.svg','w').write(f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" font-family="Helvetica,Arial"><rect width="100%" height="100%" fill="white"/>'+"".join(o)+'</svg>')
